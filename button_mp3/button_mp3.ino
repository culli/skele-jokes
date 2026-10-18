#include <Arduino.h>
#include <HardwareSerial.h>
#define SKELETON_ROLE 2
#include "SkeletonTurn.h"
#include "SkeletonEyes.h"

// Skeleton 2: Nano ESP32 + DY-SV5W, button-triggered UART control.
// D3 <- MP3 TX, D4 -> MP3 RX; button D2 -> GND.
// MP3 5V+ -> Nano VBUS (USB power); MP3 5V- and GND -> Nano GND.
// Speaker -> MP3 speaker terminals. Button connects D2 to GND.
// Each press plays the next clip, interrupting a current clip if necessary.
// Both UART wires are required: the player reports file count and play state.
// Files are selected by storage index; no filename list is required.
HardwareSerial mp3Serial(2);

const int BUTTON_PIN = D2;
const unsigned long BUTTON_DEBOUNCE_MS = 50;
const byte MP3_VOLUME = 30;

const unsigned long STATUS_POLL_MS = 200;
const unsigned long QUERY_TIMEOUT_MS = 300;
// A remote reply needs a verified name; tolerate slow/lost UART replies before
// rejecting it. Retry only the read query, never SELECT or PLAY.
const unsigned long REMOTE_QUERY_TIMEOUT_MS = 1000;
const byte MAX_REMOTE_QUERY_ATTEMPTS = 3;
const unsigned long STATUS_FRESH_MS = 1000;
const unsigned long PLAY_RETRY_MS = 600;
const unsigned long PLAY_START_TIMEOUT_MS = 1800;
const unsigned long NO_STATUS_TIMEOUT_MS = 3500;
const unsigned int MAX_TRACKS_PER_EVENT = 8;  // Bound retries on a bad card.
const unsigned long DIAGNOSTIC_TIMEOUT_MS = 600;
const byte MAX_REPLY_DATA = 60;  // Room for optional short-filename replies.
const unsigned long COMMAND_GAP_MS = 100;
// Reserve leading underscores for macOS metadata. Set false if you need real
// audio filenames starting with '_'; short names cannot distinguish the two.
const bool SKIP_UNDERSCORE_FILES = true;

enum PrepareStep { PREP_NONE, PREP_SELECT, PREP_INDEX, PREP_NAME, PREP_PLAY, PREP_SKIP };
PrepareStep prepareStep = PREP_NONE;
unsigned long prepareActionAt = 0;

struct DiagnosticReply {
  byte command = 0;
  byte length = 0;
  byte data[MAX_REPLY_DATA] = {};
  bool received = false;
};
// Status, devices, count, index, then first/last entries (at most four pairs).
DiagnosticReply startupReplies[13];
DiagnosticReply* diagnosticReply = nullptr;
unsigned int filenameSampleIndices[4] = {};
byte filenameSamples = 0;

unsigned int trackCount = 0;
unsigned int nextTrackIndex = 1;
unsigned int requestedTrack = 0;
unsigned int attemptsLeft = 0;
bool startingPlayback = false;
bool playing = false;
bool retriedPlay = false;
unsigned long playRequestedAt = 0;
bool remoteSearchActive = false;
bool remoteMatched = false;
bool remotePlaybackVerified = false;
bool remoteCacheProbe = false;
unsigned int remoteCandidate = 1;
unsigned long remoteSearchStartedAt = 0;
const unsigned long REMOTE_LOOKUP_TIMEOUT_MS = 5UL * 60 * 1000;
char requestedReplyName[SkeletonLink::NAME_SIZE] = {};
struct CachedFile { char name[SkeletonLink::NAME_SIZE] = {}; unsigned int index = 0; };
CachedFile filenameCache[16];
byte cacheSlot = 0;

bool stateKnown = false;
byte playerState = 0;  // 0 stopped, 1 playing, 2 paused.
unsigned long stateReceivedAt = 0;
byte pendingQuery = 0;
byte playbackInfoQuery = 0;  // Optional index/name queries after PLAYING status.
unsigned long querySentAt = 0;
unsigned long lastQueryAt = 0;
byte queryAttempt = 0;
unsigned int queryRxBytes = 0;
unsigned int queryBadFrames = 0;
byte rxFrame[MAX_REPLY_DATA + 4];
byte rxCount = 0;
unsigned long lastRxByteAt = 0;

bool lastButtonReading = HIGH;
bool stableButtonState = HIGH;
unsigned long buttonChangedAt = 0;

void sendCommand(byte command, byte dataLength, const byte* data);
void servicePlayer(unsigned long now);
void receivePlayer(unsigned long now);
void handleReply(unsigned long now);
void cancelPendingQuery();
void beginPlayerQuery(byte command, unsigned long now);
void printRemoteQueryTimeout();
bool playerStateFresh(unsigned long now);
void requestNextClip(unsigned long now);
void startNextAttempt(unsigned long now);
void startPreparedTrack(unsigned long now);
void servicePreparation(unsigned long now);
bool decodeFilename(const byte* data, byte length, char* name);
bool shouldSkipFilename(const char* name);
void updateButton(unsigned long now);
void runStartupDiagnostics();
void diagnosticQuery(byte command, DiagnosticReply& reply);
unsigned int diagnosticNumber(const DiagnosticReply& reply);
void printHexByte(byte value);
void printDiagnosticReply(const char* label, const DiagnosticReply& reply);
void printStartupDiagnostics();
void serviceSerialMonitor();
void failRemoteReply(const char* reason);
void rememberFilename(const char* name, unsigned int index);

void setup() {
  SkeletonEyes::begin();
  Serial.begin(115200);
  mp3Serial.begin(9600, SERIAL_8N1, D3, D4);
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  delay(1000);
  sendCommand(0x04, 0, nullptr);  // Stop any existing playback.
  delay(100);
  const byte volume[] = {MP3_VOLUME};
  sendCommand(0x13, 1, volume);
  delay(100);
  const byte mode[] = {0x02};
  sendCommand(0x18, 1, mode);  // Single track, then stop.
  delay(100);
  runStartupDiagnostics();
  printStartupDiagnostics();
  SkeletonTurn::begin();
  Serial.println("Eyes: D6 transistor control enabled; follows this skeleton's playback.");
  Serial.println("Skeleton 2: button-triggered playback; one clip per press.");
  Serial.println("Button ready when track count is available. Send d to repeat the startup report.");
}

void loop() {
  serviceSerialMonitor();
  const unsigned long now = millis();
  servicePlayer(now);
  SkeletonTurn::service(now);
  updateButton(now);
  SkeletonEyes::service(playing && stateKnown && playerState == 1 &&
      now - stateReceivedAt < SkeletonEyes::STATUS_TIMEOUT_MS, now);
}

bool playerStateFresh(unsigned long now) {
  return stateKnown && now - stateReceivedAt < STATUS_FRESH_MS;
}

void cancelPendingQuery() {
  pendingQuery = 0;
  rxCount = 0;
  while (mp3Serial.available()) mp3Serial.read();
}

void beginPlayerQuery(byte command, unsigned long now) {
  cancelPendingQuery();
  pendingQuery = command;
  querySentAt = now;
  lastQueryAt = now;
  queryAttempt = 1;
  queryRxBytes = 0;
  queryBadFrames = 0;
  sendCommand(command, 0, nullptr);
}

void printRemoteQueryTimeout() {
  Serial.print("MP3 UART timeout: query 0x"); printHexByte(pendingQuery);
  Serial.print(pendingQuery == 0x0D ? " (index)" : " (filename)");
  Serial.print(prepareStep != PREP_NONE ? " before PLAY" : " after PLAY");
  Serial.print(", requested index "); Serial.print(requestedTrack);
  Serial.print(", attempt "); Serial.print(static_cast<unsigned int>(queryAttempt));
  Serial.print('/'); Serial.print(static_cast<unsigned int>(MAX_REMOTE_QUERY_ATTEMPTS));
  Serial.print(", waited "); Serial.print(REMOTE_QUERY_TIMEOUT_MS);
  Serial.print("ms; RX bytes="); Serial.print(queryRxBytes);
  Serial.print(", invalid frames="); Serial.println(queryBadFrames);
}

void receivePlayer(unsigned long now) {
  if (rxCount && now - lastRxByteAt > 100) rxCount = 0;
  while (mp3Serial.available()) {
    const byte value = static_cast<byte>(mp3Serial.read());
    if (pendingQuery) ++queryRxBytes;
    lastRxByteAt = now;
    if (rxCount == 0 && value != 0xAA) continue;
    rxFrame[rxCount++] = value;
    if (rxCount == 3 && rxFrame[2] > sizeof(rxFrame) - 4) {
      if (pendingQuery) ++queryBadFrames;
      rxCount = 0;
      continue;
    }
    if (rxCount >= 4 && rxCount == 4 + rxFrame[2]) {
      byte checksum = 0;
      for (byte i = 0; i < rxCount - 1; ++i) checksum += rxFrame[i];
      if (checksum == rxFrame[rxCount - 1] && rxFrame[1] == pendingQuery) {
        handleReply(now);
      } else if (pendingQuery && checksum != rxFrame[rxCount - 1]) {
        ++queryBadFrames;
      }
      rxCount = 0;
    }
  }
}

void handleReply(unsigned long now) {
  if (diagnosticReply != nullptr) {
    diagnosticReply->length = rxFrame[2];
    for (byte i = 0; i < rxFrame[2]; ++i) diagnosticReply->data[i] = rxFrame[3 + i];
    diagnosticReply->received = true;
    pendingQuery = 0;
    return;
  }
  if (prepareStep != PREP_NONE && pendingQuery == 0x0D && rxFrame[2] == 2) {
    pendingQuery = 0;
    const unsigned int selected = (static_cast<unsigned int>(rxFrame[3]) << 8) | rxFrame[4];
    prepareActionAt = now;
    if (selected == requestedTrack) {
      prepareStep = PREP_NAME;
    } else {
      if (remoteSearchActive) {
        failRemoteReply("Could not verify the selected file index.");
        return;
      }
      Serial.println("Selection was not confirmed; using direct indexed playback without filename filtering.");
      prepareStep = PREP_PLAY;
    }
    return;
  }
  if (prepareStep != PREP_NONE && pendingQuery == 0x1E) {
    pendingQuery = 0;
    prepareActionAt = now;
    prepareStep = PREP_PLAY;
    char filename[MAX_REPLY_DATA + 1];
    if (decodeFilename(rxFrame + 3, rxFrame[2], filename)) {
      Serial.print("Selected filename: "); Serial.println(filename);
      char normalized[SkeletonLink::NAME_SIZE];
      const bool validName = SkeletonLink::normalizeName(filename, normalized);
      if (validName) rememberFilename(normalized, requestedTrack);
      if (remoteSearchActive) {
        if (validName && strcmp(normalized, requestedReplyName) == 0) {
          remoteMatched = true;
          remoteCacheProbe = false;
        } else {
          if (remoteCacheProbe) {
            remoteCacheProbe = false;
            remoteCandidate = 1;
            attemptsLeft = trackCount;
          }
          prepareStep = PREP_SKIP;
        }
      }
      if (shouldSkipFilename(filename)) {
        Serial.print("Skipping metadata-style filename "); Serial.print(filename);
        Serial.print(" at index "); Serial.println(requestedTrack);
        prepareStep = PREP_SKIP;
      }
    } else {
      if (remoteSearchActive) {
        failRemoteReply("Could not read the filename during lookup.");
        return;
      }
      Serial.println("Filename format not recognized; using existing playback fallback.");
    }
    return;
  }
  if (pendingQuery == 0x0C && rxFrame[2] == 2) {
    trackCount = (static_cast<unsigned int>(rxFrame[3]) << 8) | rxFrame[4];
    pendingQuery = 0;
    if (trackCount) {
      Serial.print("MP3 reports ");
      Serial.print(trackCount);
      Serial.println(" audio entries. Button ready.");
    } else {
      Serial.println("MP3 reports no audio entries. Check the card.");
    }
  } else if (pendingQuery == 0x0D && rxFrame[2] == 2) {
    pendingQuery = 0;
    const unsigned int reportedIndex = (static_cast<unsigned int>(rxFrame[3]) << 8) | rxFrame[4];
    Serial.print("Index reported after PLAY: "); Serial.print(reportedIndex);
    Serial.print(" (requested "); Serial.print(requestedTrack); Serial.println(')');
    if (reportedIndex != requestedTrack) {
      if (remoteSearchActive) {
        failRemoteReply("Player changed to a different index instead of the matching reply.");
        return;
      }
      Serial.println("Reported index differs from the request; the module may have skipped an entry.");
      if (reportedIndex >= 1 && reportedIndex <= trackCount) {
        nextTrackIndex = reportedIndex >= trackCount ? 1 : reportedIndex + 1;
        Serial.print("Next event will continue at index "); Serial.println(nextTrackIndex);
      }
    }
  } else if (pendingQuery == 0x1E) {
    pendingQuery = 0;
    DiagnosticReply reply;
    reply.command = 0x1E;
    reply.length = rxFrame[2];
    reply.received = true;
    for (byte i = 0; i < reply.length; ++i) reply.data[i] = rxFrame[3 + i];
    printDiagnosticReply("Filename reported after PLAY", reply);
    if (remoteSearchActive) {
      char decoded[MAX_REPLY_DATA + 1];
      char normalized[SkeletonLink::NAME_SIZE];
      remotePlaybackVerified = decodeFilename(reply.data, reply.length, decoded) &&
          SkeletonLink::normalizeName(decoded, normalized) && strcmp(normalized, requestedReplyName) == 0;
      if (!remotePlaybackVerified) {
        failRemoteReply("Player did not report the requested filename after PLAY.");
        return;
      }
      SkeletonTurn::playbackStarted();
    }
  } else if (pendingQuery == 0x01 && rxFrame[2] == 1 && rxFrame[3] <= 2) {
    pendingQuery = 0;
    playerState = rxFrame[3];
    stateKnown = true;
    stateReceivedAt = now;
    if (playerState == 1) {
      if (startingPlayback) {
        Serial.print("Player reports PLAYING (requested index ");
        Serial.print(requestedTrack); Serial.println("). Querying index and filename...");
        playbackInfoQuery = 0x0D;
      }
      startingPlayback = false;
      playing = true;
    } else if (playing) {
      playing = false;
      playbackInfoQuery = 0;
      if (playerState == 0) {
        Serial.println("Clip finished. Press the button for the next clip.");
        if (remoteSearchActive) {
          remoteSearchActive = false;
          SkeletonTurn::finishRemote(remotePlaybackVerified);
        }
      } else {
        if (remoteSearchActive) {
          failRemoteReply("Reply playback paused before it finished.");
          return;
        }
        Serial.println("MP3 is paused. Press the button to select the next clip.");
      }
    }
  }
}

void servicePlayer(unsigned long now) {
  receivePlayer(now);
  if (remoteSearchActive && !remoteMatched && now - remoteSearchStartedAt >= REMOTE_LOOKUP_TIMEOUT_MS) {
    failRemoteReply("Filename lookup timed out.");
    return;
  }
  const bool remoteInfoQuery = remoteSearchActive && (pendingQuery == 0x0D || pendingQuery == 0x1E);
  const unsigned long queryTimeout = remoteInfoQuery ? REMOTE_QUERY_TIMEOUT_MS : QUERY_TIMEOUT_MS;
  if (pendingQuery && now - querySentAt >= queryTimeout) {
    if (remoteInfoQuery) {
      printRemoteQueryTimeout();
      if (queryAttempt < MAX_REMOTE_QUERY_ATTEMPTS) {
        const byte command = pendingQuery;
        const byte nextAttempt = queryAttempt + 1;
        Serial.println("Retrying the same MP3 query; no PLAY command sent by this retry.");
        beginPlayerQuery(command, now);
        queryAttempt = nextAttempt;
        return;
      }
      failRemoteReply("MP3 index/filename query failed after 3 attempts; no filename guess made. Send d for this board's startup report.");
      return;
    }
    if (prepareStep != PREP_NONE) {
      Serial.println("Filename check timed out; using existing playback fallback.");
      prepareStep = PREP_PLAY;
      prepareActionAt = now;
    } else if (pendingQuery == 0x0D || pendingQuery == 0x1E) {
      Serial.print("No valid reply to playback information query 0x");
      printHexByte(pendingQuery); Serial.println("; continuing normal playback.");
    }
    pendingQuery = 0;
    rxCount = 0;
  }

  if (prepareStep != PREP_NONE) {
    servicePreparation(now);
    return;
  }

  if (startingPlayback) {
    const unsigned long elapsed = now - playRequestedAt;
    // Retry PLAY only when a fresh reply still says stopped, avoiding a
    // second command that would restart an already playing clip.
    if (playerStateFresh(now) && playerState == 0) {
      if (elapsed >= PLAY_START_TIMEOUT_MS) {
        if (remoteSearchActive) {
          failRemoteReply("The matching reply file did not start.");
          return;
        }
        Serial.print("Index ");
        Serial.print(requestedTrack);
        Serial.println(" did not start; trying the next entry within this event.");
        startNextAttempt(now);
      } else if (!retriedPlay && elapsed >= PLAY_RETRY_MS) {
        cancelPendingQuery();
        sendCommand(0x02, 0, nullptr);
        retriedPlay = true;
        stateKnown = false;
        lastQueryAt = now;
        Serial.println("Player still stopped; sending explicit PLAY once.");
      }
    } else if (elapsed >= NO_STATUS_TIMEOUT_MS) {
      if (remoteSearchActive) {
        failRemoteReply("Reply playback could not be confirmed.");
        return;
      }
      startingPlayback = false;
      Serial.println("Playback not confirmed. Check MP3 TX -> D3 and common GND; press the button to retry.");
    }
  }

  // One query at a time. Receive processing stays nonblocking for the button.
  const unsigned long interval = trackCount ? STATUS_POLL_MS : 1000;
  if (!pendingQuery && now - lastQueryAt >= interval) {
    byte command;
    if (playing && playbackInfoQuery) {
      command = playbackInfoQuery;
      playbackInfoQuery = playbackInfoQuery == 0x0D ? 0x1E : 0;
    } else {
      command = trackCount ? 0x01 : 0x0C;
    }
    beginPlayerQuery(command, now);
  }
}

void requestNextClip(unsigned long now) {
  if (!trackCount) {
    Serial.println("No track count yet. Check card, MP3 TX -> D3, and common GND.");
    return;
  }
  attemptsLeft = trackCount < MAX_TRACKS_PER_EVENT ? trackCount : MAX_TRACKS_PER_EVENT;
  startNextAttempt(now);
}

void startNextAttempt(unsigned long now) {
  cancelPendingQuery();
  playbackInfoQuery = 0;
  prepareStep = PREP_NONE;
  if (attemptsLeft == 0) {
    if (remoteSearchActive) {
      failRemoteReply("Matching filename was not found on this card.");
      return;
    }
    startingPlayback = false;
    Serial.println("No clip started in this event. Check audio files/card; button can try again.");
    return;
  }
  --attemptsLeft;
  if (remoteSearchActive) requestedTrack = remoteCandidate++;
  else {
    requestedTrack = nextTrackIndex;
    nextTrackIndex = nextTrackIndex >= trackCount ? 1 : nextTrackIndex + 1;
  }
  sendCommand(0x04, 0, nullptr);  // A button event may interrupt a playing clip.
  prepareStep = PREP_SELECT;
  prepareActionAt = now;
  startingPlayback = false;
  playing = false;
  stateKnown = false;
  Serial.print("Requesting audio index ");
  Serial.print(requestedTrack);
  Serial.print(" of ");
  Serial.println(trackCount);
}

void servicePreparation(unsigned long now) {
  if (pendingQuery || now - prepareActionAt < COMMAND_GAP_MS) return;
  prepareActionAt = now;
  if (prepareStep == PREP_SELECT) {
    const byte data[] = {highByte(requestedTrack), lowByte(requestedTrack)};
    sendCommand(0x1F, 2, data);  // Read the selected name before any PLAY command.
    prepareStep = PREP_INDEX;
  } else if (prepareStep == PREP_INDEX || prepareStep == PREP_NAME) {
    beginPlayerQuery(prepareStep == PREP_INDEX ? 0x0D : 0x1E, now);
  } else if (prepareStep == PREP_SKIP) {
    startNextAttempt(now);
  } else if (prepareStep == PREP_PLAY) {
    startPreparedTrack(now);
  }
}

void startPreparedTrack(unsigned long now) {
  prepareStep = PREP_NONE;
  cancelPendingQuery();
  const byte data[] = {highByte(requestedTrack), lowByte(requestedTrack)};
  if (remoteSearchActive) nextTrackIndex = requestedTrack >= trackCount ? 1 : requestedTrack + 1;
  sendCommand(0x07, 2, data);  // Select this index AND play, not NEXT alone.
  startingPlayback = true;
  playing = false;
  stateKnown = false;
  retriedPlay = false;
  playRequestedAt = now;
  lastQueryAt = now;
  Serial.print("Starting audio index "); Serial.println(requestedTrack);
}

bool decodeFilename(const byte* data, byte length, char* name) {
  // Caller supplies MAX_REPLY_DATA + 1 bytes. Accept the observed 8+3+NUL
  // format, plus dotted text returned by some variants. Unknown data is allowed
  // through to playback rather than treated as a reason to skip the file.
  if (!length || length > MAX_REPLY_DATA) return false;
  byte count = 0;
  bool dotted = false;
  while (count < length && data[count] != 0) {
    if (data[count] < 32 || data[count] > 126 || data[count] == '/' || data[count] == '\\') return false;
    if (data[count] == '.') dotted = true;
    ++count;
  }
  for (byte i = count; i < length; ++i) if (data[i] != 0) return false;
  if (count == 11 && !dotted) {
    byte baseEnd = 8;
    while (baseEnd && data[baseEnd - 1] == ' ') --baseEnd;
    byte extEnd = 11;
    while (extEnd > 8 && data[extEnd - 1] == ' ') --extEnd;
    if (!baseEnd || extEnd == 8) return false;
    byte out = 0;
    for (byte i = 0; i < baseEnd; ++i) {
      if (data[i] == ' ') return false;
      name[out++] = data[i];
    }
    name[out++] = '.';
    for (byte i = 8; i < extEnd; ++i) {
      if (data[i] == ' ') return false;
      name[out++] = data[i];
    }
    name[out] = '\0';
    return true;
  }
  while (count && data[count - 1] == ' ') --count;
  if (!count || !dotted || data[0] == ' ' || data[count - 1] == '.') return false;
  for (byte i = 0; i < count; ++i) name[i] = data[i];
  name[count] = '\0';
  return true;
}

bool shouldSkipFilename(const char* name) {
  return (name[0] == '.' && name[1] == '_') || (SKIP_UNDERSCORE_FILES && name[0] == '_');
}

bool playerBusyForReply() {
  return playing || startingPlayback || prepareStep != PREP_NONE;
}

void rememberFilename(const char* name, unsigned int index) {
  for (CachedFile& entry : filenameCache) {
    if (entry.index && strcmp(entry.name, name) == 0) { entry.index = index; return; }
  }
  strcpy(filenameCache[cacheSlot].name, name);
  filenameCache[cacheSlot].index = index;
  cacheSlot = (cacheSlot + 1) % 16;
}

void startRemoteReply(const char* filename, unsigned long now) {
  if (!trackCount) {
    SkeletonTurn::finishRemote(false);
    Serial.println("No card/file count available for the Bluetooth reply.");
    return;
  }
  strcpy(requestedReplyName, filename);  // Validated, bounded protocol name.
  remoteSearchActive = true;
  remoteMatched = false;
  remotePlaybackVerified = false;
  remoteCandidate = 1;
  remoteCacheProbe = false;
  remoteSearchStartedAt = now;
  for (const CachedFile& entry : filenameCache) {
    if (entry.index && entry.index <= trackCount && strcmp(entry.name, filename) == 0) {
      remoteCandidate = entry.index;
      remoteCacheProbe = true;
      break;
    }
  }
  attemptsLeft = remoteCacheProbe ? 1 : trackCount;
  Serial.print("BLE: finding exact filename "); Serial.println(filename);
  startNextAttempt(now);
}

void failRemoteReply(const char* reason) {
  cancelPendingQuery();
  sendCommand(0x04, 0, nullptr);
  prepareStep = PREP_NONE;
  startingPlayback = false;
  playing = false;
  stateKnown = false;
  playbackInfoQuery = 0;
  remoteSearchActive = false;
  Serial.print("BLE reply error: "); Serial.println(reason);
  SkeletonTurn::finishRemote(false);
}

void updateButton(unsigned long now) {
  const bool reading = digitalRead(BUTTON_PIN);
  if (reading != lastButtonReading) {
    lastButtonReading = reading;
    buttonChangedAt = now;
  }
  if (reading != stableButtonState && now - buttonChangedAt >= BUTTON_DEBOUNCE_MS) {
    stableButtonState = reading;
    if (reading == LOW) {
      if (!SkeletonTurn::allowButton(now)) return;
      Serial.println("Button: request next clip.");
      requestNextClip(now);  // Manual skip is allowed even during playback.
    }
  }
}

void diagnosticQuery(byte command, DiagnosticReply& reply) {
  // Only called during setup. Unsupported queries cannot stall normal operation.
  delay(100);
  cancelPendingQuery();
  reply = DiagnosticReply{};
  reply.command = command;
  diagnosticReply = &reply;
  pendingQuery = command;
  const unsigned long started = millis();
  sendCommand(command, 0, nullptr);
  while (!reply.received && millis() - started < DIAGNOSTIC_TIMEOUT_MS) {
    receivePlayer(millis());
    delay(1);
  }
  diagnosticReply = nullptr;
  cancelPendingQuery();
}

unsigned int diagnosticNumber(const DiagnosticReply& reply) {
  if (!reply.received || reply.length != 2) return 0;
  return (static_cast<unsigned int>(reply.data[0]) << 8) | reply.data[1];
}

void runStartupDiagnostics() {
  // The published DY UART protocol has no firmware-version command.
  // 0x1E is an optional filename probe, not a firmware-version query.
  const byte queries[] = {0x01, 0x09, 0x0A, 0x0C, 0x0D};
  for (byte i = 0; i < sizeof(queries); ++i) diagnosticQuery(queries[i], startupReplies[i]);
  trackCount = diagnosticNumber(startupReplies[3]);
  filenameSamples = 0;
  if (trackCount >= 1) filenameSampleIndices[filenameSamples++] = 1;
  if (trackCount >= 2) filenameSampleIndices[filenameSamples++] = 2;
  if (trackCount >= 4) filenameSampleIndices[filenameSamples++] = trackCount - 1;
  if (trackCount >= 3) filenameSampleIndices[filenameSamples++] = trackCount;
  for (byte i = 0; i < filenameSamples; ++i) {
    const byte selection[] = {highByte(filenameSampleIndices[i]), lowByte(filenameSampleIndices[i])};
    sendCommand(0x1F, 2, selection);  // Select without playing.
    diagnosticQuery(0x0D, startupReplies[5 + 2 * i]);
    diagnosticQuery(0x1E, startupReplies[6 + 2 * i]);
  }
  if (filenameSamples) {
    unsigned int restoreIndex = diagnosticNumber(startupReplies[4]);
    if (!restoreIndex || restoreIndex > trackCount) restoreIndex = 1;
    const byte selection[] = {highByte(restoreIndex), lowByte(restoreIndex)};
    delay(100);
    sendCommand(0x1F, 2, selection);
  }
  // Normal status polling resumes in loop; the first button press still
  // explicitly requests index 1. Diagnostics never send a PLAY command.
  cancelPendingQuery();
  stateKnown = false;
  lastQueryAt = millis();
}

void printHexByte(byte value) {
  const char hex[] = "0123456789ABCDEF";
  Serial.print(hex[value >> 4]);
  Serial.print(hex[value & 0x0F]);
}

void printDiagnosticReply(const char* label, const DiagnosticReply& reply) {
  Serial.print(label); Serial.print(" (0x"); printHexByte(reply.command); Serial.print("): ");
  if (!reply.received) {
    Serial.println("no valid reply within 600ms");
    return;
  }
  // Display the complete checksum-validated frame, including non-text bytes.
  Serial.print("AA "); printHexByte(reply.command); Serial.print(' '); printHexByte(reply.length);
  byte checksum = 0xAA + reply.command + reply.length;
  for (byte i = 0; i < reply.length; ++i) {
    Serial.print(' '); printHexByte(reply.data[i]); checksum += reply.data[i];
  }
  Serial.print(' '); printHexByte(checksum);
  if (reply.command == 0x1E) {
    Serial.print(" | text=\"");
    for (byte i = 0; i < reply.length; ++i) {
      const byte value = reply.data[i];
      Serial.print(value >= 32 && value <= 126 ? static_cast<char>(value) : '.');
    }
    Serial.print('"');
  } else if (reply.command == 0x0C || reply.command == 0x0D) {
    if (reply.length == 2) {
      Serial.print(reply.command == 0x0C ? " | entries=" : " | index=");
      Serial.print(diagnosticNumber(reply));
    } else Serial.print(" | unexpected payload length");
  } else if (reply.length != 1) {
    Serial.print(" | unexpected payload length");
  } else if (reply.command == 0x01) {
    const byte state = reply.data[0];
    Serial.print(state == 0 ? " | stopped" : state == 1 ? " | playing" : state == 2 ? " | paused" : " | unknown state");
  } else if (reply.command == 0x0A) {
    const byte device = reply.data[0];
    Serial.print(device == 0 ? " | USB" : device == 1 ? " | SD" : device == 2 ? " | flash" : device == 0xFF ? " | no device" : " | unknown device");
  }
  Serial.println();
}

void printStartupDiagnostics() {
  Serial.println("\n=== Skeleton 2 / DY-SV5W startup diagnostics (saved snapshot) ===");
  Serial.println("Sketch build: " __DATE__ " " __TIME__);
  Serial.println("MP3 firmware version: unknown; no documented UART version query.");
  Serial.println("UART: 9600 8N1; MP3 TX -> D3, MP3 RX <- D4; volume requested: 30.");
  const char* labels[] = {"Playback state", "Online devices (raw)", "Current device", "Audio entries", "Initial index"};
  for (byte i = 0; i < 5; ++i) printDiagnosticReply(labels[i], startupReplies[i]);
  for (byte i = 0; i < filenameSamples; ++i) {
    Serial.print("Selected index "); Serial.print(filenameSampleIndices[i]); Serial.println(" with 0x1F (no PLAY sent):");
    printDiagnosticReply("  Reported index", startupReplies[5 + 2 * i]);
    printDiagnosticReply("  Optional short filename", startupReplies[6 + 2 * i]);
  }
  if (!filenameSamples) Serial.println("Filename probe skipped: no positive track count received.");
  Serial.println("Before playback: verify selected index, query filename, skip ._ names.");
  Serial.println(SKIP_UNDERSCORE_FILES ? "Leading _ names are also skipped (reserved for metadata)." : "Leading _ names are allowed; underscore filtering is disabled.");
  Serial.println("Local button playback can fall back if filename replies are unavailable.");
  Serial.println("Bluetooth replies require a verified match; index/name queries allow 1000ms per attempt, up to 3 attempts.");
  Serial.println("=== End startup diagnostics; send d to display again ===\n");
}

void serviceSerialMonitor() {
  // Reprint cached results without changing the selected track or playback.
  if (Serial.available()) {
    const int input = Serial.read();
    if (input == 'd' || input == 'D') printStartupDiagnostics();
  }
}

void sendCommand(byte command, byte dataLength, const byte* data) {
  byte frame[6];
  if (dataLength > sizeof(frame) - 4 || (dataLength && data == nullptr)) {
    Serial.println("Invalid MP3 command data.");
    return;
  }
  frame[0] = 0xAA;
  frame[1] = command;
  frame[2] = dataLength;
  for (byte i = 0; i < dataLength; ++i) frame[3 + i] = data[i];
  const byte length = 3 + dataLength;
  byte checksum = 0;
  for (byte i = 0; i < length; ++i) checksum += frame[i];
  frame[length] = checksum;
  mp3Serial.write(frame, length + 1);
}
