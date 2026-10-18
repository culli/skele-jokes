#include <Arduino.h>
#include <HardwareSerial.h>
#define SKELETON_ROLE 1
#include "SkeletonTurn.h"
#include "SkeletonEyes.h"

// Nano ESP32 + DY-SV5W + HC-SR501, UART control mode.
// D3 <- MP3 TX, D4 -> MP3 RX; button D2 -> GND.
// PIR VCC -> VBUS (USB 5V), GND -> common GND, OUT -> D5.
// Both UART wires are required: the player reports file count and play state.
// Files are selected by storage index; no filename list is required.
HardwareSerial mp3Serial(2);

const int BUTTON_PIN = D2;
const int PIR_PIN = D5;
const unsigned long BUTTON_DEBOUNCE_MS = 50;
const unsigned long PIR_WARMUP_MS = 60000;
const unsigned long PIR_HIGH_MS = 80;
const unsigned long MOTION_GAP_MS = 2000;  // Gap AFTER a clip finishes.
const byte MP3_VOLUME = 30;
const bool PIR_DIAGNOSTICS = true;
const unsigned long PIR_REPORT_MS = 2000;

const unsigned long STATUS_POLL_MS = 200;
const unsigned long QUERY_TIMEOUT_MS = 300;
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
bool motionFault = false;
bool gapActive = false;
bool localClipConfirmed = false;
bool playedFilenameVerified = false;
char playedFilename[SkeletonLink::NAME_SIZE] = {};
unsigned long playRequestedAt = 0;
unsigned long playbackEndedAt = 0;

bool stateKnown = false;
byte playerState = 0;  // 0 stopped, 1 playing, 2 paused.
unsigned long stateReceivedAt = 0;
byte pendingQuery = 0;
byte playbackInfoQuery = 0;  // Optional index/name queries after PLAYING status.
unsigned long querySentAt = 0;
unsigned long lastQueryAt = 0;
byte rxFrame[MAX_REPLY_DATA + 4];
byte rxCount = 0;
unsigned long lastRxByteAt = 0;

unsigned long pirStartTime = 0;
bool pirReady = false;
bool previousMotion = false;
unsigned long pirHighSince = 0;
bool lastButtonReading = HIGH;
bool stableButtonState = HIGH;
unsigned long buttonChangedAt = 0;

void sendCommand(byte command, byte dataLength, const byte* data);
void servicePlayer(unsigned long now);
void receivePlayer(unsigned long now);
void handleReply(unsigned long now);
void cancelPendingQuery();
bool playerStateFresh(unsigned long now);
unsigned long motionGapLeft(unsigned long now);
void requestNextClip(unsigned long now);
void startNextAttempt(unsigned long now);
void startPreparedTrack(unsigned long now);
void servicePreparation(unsigned long now);
bool decodeFilename(const byte* data, byte length, char* name);
bool shouldSkipFilename(const char* name);
void updateButton(unsigned long now);
void updateMotion(unsigned long now);
void reportMotionStatus(unsigned long now);
void runStartupDiagnostics();
void diagnosticQuery(byte command, DiagnosticReply& reply);
unsigned int diagnosticNumber(const DiagnosticReply& reply);
void printHexByte(byte value);
void printDiagnosticReply(const char* label, const DiagnosticReply& reply);
void printStartupDiagnostics();
void serviceSerialMonitor();

void setup() {
  SkeletonEyes::begin();
  Serial.begin(115200);
  mp3Serial.begin(9600, SERIAL_8N1, D3, D4);
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(PIR_PIN, INPUT_PULLDOWN);
  pirStartTime = millis();
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
  Serial.println("Button ready when track count is available. Send d to repeat the startup report.");
  Serial.println("PIR warming up for 60s. Continued HIGH can play again after each clip + 2s.");
}

void loop() {
  serviceSerialMonitor();
  const unsigned long now = millis();
  SkeletonTurn::service(now);
  servicePlayer(now);
  updateButton(now);
  updateMotion(now);
  SkeletonEyes::service(playing && stateKnown && playerState == 1 &&
      now - stateReceivedAt < SkeletonEyes::STATUS_TIMEOUT_MS, now);
  if (PIR_DIAGNOSTICS) reportMotionStatus(now);
}

bool playerStateFresh(unsigned long now) {
  return stateKnown && now - stateReceivedAt < STATUS_FRESH_MS;
}

unsigned long motionGapLeft(unsigned long now) {
  if (!gapActive || now - playbackEndedAt >= MOTION_GAP_MS) return 0;
  return MOTION_GAP_MS - (now - playbackEndedAt);
}

void cancelPendingQuery() {
  pendingQuery = 0;
  rxCount = 0;
  while (mp3Serial.available()) mp3Serial.read();
}

void receivePlayer(unsigned long now) {
  if (rxCount && now - lastRxByteAt > 100) rxCount = 0;
  while (mp3Serial.available()) {
    const byte value = static_cast<byte>(mp3Serial.read());
    lastRxByteAt = now;
    if (rxCount == 0 && value != 0xAA) continue;
    rxFrame[rxCount++] = value;
    if (rxCount == 3 && rxFrame[2] > sizeof(rxFrame) - 4) {
      rxCount = 0;
      continue;
    }
    if (rxCount >= 4 && rxCount == 4 + rxFrame[2]) {
      byte checksum = 0;
      for (byte i = 0; i < rxCount - 1; ++i) checksum += rxFrame[i];
      if (checksum == rxFrame[rxCount - 1] && rxFrame[1] == pendingQuery) {
        handleReply(now);
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
      if (shouldSkipFilename(filename)) {
        Serial.print("Skipping metadata-style filename "); Serial.print(filename);
        Serial.print(" at index "); Serial.println(requestedTrack);
        prepareStep = PREP_SKIP;
      }
    } else {
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
      playedFilenameVerified = false;
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
    char decoded[MAX_REPLY_DATA + 1];
    playedFilenameVerified = decodeFilename(reply.data, reply.length, decoded) &&
        SkeletonLink::normalizeName(decoded, playedFilename);
  } else if (pendingQuery == 0x01 && rxFrame[2] == 1 && rxFrame[3] <= 2) {
    pendingQuery = 0;
    playerState = rxFrame[3];
    stateKnown = true;
    stateReceivedAt = now;
    if (playerState == 1) {
      if (startingPlayback) {
        localClipConfirmed = true;
        Serial.print("Player reports PLAYING (requested index ");
        Serial.print(requestedTrack); Serial.println("). Querying index and filename...");
        playbackInfoQuery = 0x0D;
      }
      startingPlayback = false;
      playing = true;
      gapActive = false;
      motionFault = false;
    } else if (playing) {
      playing = false;
      playbackInfoQuery = 0;
      if (playerState == 0) {
        playbackEndedAt = now;
        gapActive = true;
        Serial.println("Clip finished. Skeleton 2 gets the next turn.");
        if (localClipConfirmed) {
          SkeletonTurn::completeLocalPlayback(playedFilenameVerified ? playedFilename : "", now);
        }
      } else {
        Serial.println("MP3 is paused. Press the button to select the next clip.");
      }
      localClipConfirmed = false;
      playedFilenameVerified = false;
    }
  }
}

void servicePlayer(unsigned long now) {
  receivePlayer(now);
  if (pendingQuery && now - querySentAt >= QUERY_TIMEOUT_MS) {
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
      startingPlayback = false;
      motionFault = true;
      Serial.println("Playback not confirmed. Check MP3 TX -> D3 and common GND; motion paused.");
    }
  }

  // One query at a time. Receive processing stays nonblocking for the button.
  const unsigned long interval = trackCount ? STATUS_POLL_MS : 1000;
  if (!pendingQuery && now - lastQueryAt >= interval) {
    if (playing && playbackInfoQuery) {
      pendingQuery = playbackInfoQuery;
      playbackInfoQuery = playbackInfoQuery == 0x0D ? 0x1E : 0;
    } else {
      pendingQuery = trackCount ? 0x01 : 0x0C;
    }
    querySentAt = now;
    lastQueryAt = now;
    rxCount = 0;
    sendCommand(pendingQuery, 0, nullptr);
  }
}

void requestNextClip(unsigned long now) {
  if (!trackCount) {
    Serial.println("No track count yet. Check card, MP3 TX -> D3, and common GND.");
    return;
  }
  attemptsLeft = trackCount < MAX_TRACKS_PER_EVENT ? trackCount : MAX_TRACKS_PER_EVENT;
  motionFault = false;
  startNextAttempt(now);
}

void startNextAttempt(unsigned long now) {
  // A manual skip cancels the old clip; only a natural finish sends a reply.
  localClipConfirmed = false;
  playedFilenameVerified = false;
  playedFilename[0] = 0;
  cancelPendingQuery();
  playbackInfoQuery = 0;
  prepareStep = PREP_NONE;
  if (attemptsLeft == 0) {
    startingPlayback = false;
    motionFault = true;
    Serial.println("No clip started in this event. Check audio files/card; button can try again.");
    return;
  }
  --attemptsLeft;
  requestedTrack = nextTrackIndex;
  nextTrackIndex = nextTrackIndex >= trackCount ? 1 : nextTrackIndex + 1;
  sendCommand(0x04, 0, nullptr);  // A button event may interrupt a playing clip.
  prepareStep = PREP_SELECT;
  prepareActionAt = now;
  startingPlayback = false;
  playing = false;
  stateKnown = false;
  gapActive = false;
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
    pendingQuery = prepareStep == PREP_INDEX ? 0x0D : 0x1E;
    querySentAt = now;
    rxCount = 0;
    sendCommand(pendingQuery, 0, nullptr);
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
  sendCommand(0x07, 2, data);  // Select this index AND play, not NEXT alone.
  startingPlayback = true;
  playing = false;
  stateKnown = false;
  retriedPlay = false;
  gapActive = false;
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

void updateMotion(unsigned long now) {
  const bool motion = digitalRead(PIR_PIN) == HIGH;
  if (motion && !previousMotion) pirHighSince = now;
  previousMotion = motion;
  if (!pirReady) {
    if (now - pirStartTime < PIR_WARMUP_MS) return;
    pirReady = true;
    Serial.println("PIR ready. No LOW/rearm wait; HIGH may repeat after playback ends.");
  }
  if (motion && now - pirHighSince >= PIR_HIGH_MS &&
      SkeletonTurn::idle() && trackCount && prepareStep == PREP_NONE && !startingPlayback && !playing && !motionFault &&
      playerStateFresh(now) && playerState == 0 && motionGapLeft(now) == 0) {
    Serial.println("Motion: request next clip.");
    requestNextClip(now);
  }
}

void reportMotionStatus(unsigned long now) {
  static int previousReading = -1;
  static unsigned long stateSince = 0;
  static unsigned long lastReportTime = 0;
  const int reading = digitalRead(PIR_PIN);
  const bool changed = reading != previousReading;
  if (changed) {
    previousReading = reading;
    stateSince = now;
  }
  if (!changed && now - lastReportTime < PIR_REPORT_MS) return;
  lastReportTime = now;
  Serial.print("["); Serial.print(now / 1000); Serial.print("s] PIR=");
  Serial.print(reading == HIGH ? "HIGH" : "LOW");
  Serial.print(" for "); Serial.print((now - stateSince) / 1000); Serial.print("s | ");
  if (!pirReady) Serial.print("warming up | ");
  if (!trackCount) Serial.print("waiting for MP3 track count (check TX -> D3)");
  else if (!SkeletonTurn::idle()) Serial.print("waiting for skeleton 2 / BLE reply; see messages above");
  else if (motionFault) Serial.print("motion paused: playback error; see messages above");
  else if (prepareStep != PREP_NONE) Serial.print("checking selected filename before playback");
  else if (startingPlayback) Serial.print("waiting for playback to start");
  else if (!playerStateFresh(now)) Serial.print("waiting for MP3 status (check TX -> D3)");
  else if (playing) Serial.print("clip playing");
  else if (playerState == 2) Serial.print("MP3 paused");
  else if (motionGapLeft(now)) {
    Serial.print("gap: "); Serial.print((motionGapLeft(now) + 999) / 1000); Serial.print("s");
  } else Serial.print("ready for motion");
  Serial.println();
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
  // Normal status polling resumes in loop; the first button/motion event still
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
  Serial.println("\n=== DY-SV5W startup diagnostics (saved snapshot) ===");
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
  Serial.println("A missing/unrecognized filename reply falls back to normal indexed playback.");
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
