#pragma once
#include "SkeletonBLE.h"
#if SKELETON_ROLE == 1
#include <esp_system.h>
#else
#include <Preferences.h>
bool playerBusyForReply();
void startRemoteReply(const char* filename, unsigned long now);
#endif

namespace SkeletonTurn {
using SkeletonLink::Packet;
constexpr unsigned long DELIVERY_TIMEOUT_MS = 30000;
constexpr unsigned long REPLY_TIMEOUT_MS = 15UL * 60 * 1000;
static bool previousConnection = false;

#if SKELETON_ROLE == 1
enum TurnState { IDLE, WAITING, FAULT };
static TurnState state = IDLE;
static Packet request;
static uint32_t bootSession = 0;
static uint16_t nextSequence = 0;
static unsigned long waitingSince = 0;
static unsigned long lastSend = 0;
static bool acknowledged = false;
static uint8_t lastStatus = 0;
inline bool idle() { return state == IDLE; }

inline void begin() {
  bootSession = esp_random();
  if (!bootSession) bootSession = 1;
  if (SkeletonLink::begin()) Serial.println("BLE: skeleton 1 is looking for skeleton 2.");
  else Serial.println("BLE initialization failed. Restart this Arduino.");
}

inline void completeLocalPlayback(const char* filename, unsigned long now) {
  if (!idle()) return;
  Packet next;
  if (!SkeletonLink::normalizeName(filename, next.name)) {
    Serial.println("No verified short filename to send; partner reply skipped.");
    return;
  }
  if (++nextSequence == 0) {
    ++bootSession;
    if (!bootSession) bootSession = 1;
    nextSequence = 1;
  }
  next.session = bootSession;
  next.sequence = nextSequence;
  request = next;
  state = WAITING;
  waitingSince = now;
  lastSend = now - 1000;
  acknowledged = false;
  lastStatus = 0;
  Serial.print("BLE: completed "); Serial.print(request.name);
  Serial.println("; requesting skeleton 2's matching reply.");
}

inline bool allowButton(unsigned long now) {
  if (state == IDLE) return true;
  if (state == FAULT) {
    state = WAITING;
    waitingSince = now;
    lastSend = now - 1000;
    acknowledged = false;
    Serial.println("BLE: retrying the same reply request; duplicate playback is suppressed.");
  } else Serial.println("Skeleton 2's turn. Waiting for its reply to finish.");
  return false;
}

inline void service(unsigned long now) {
  SkeletonLink::service();
  const bool connected = SkeletonLink::isConnected();
  if (connected != previousConnection) {
    previousConnection = connected;
    Serial.println(connected ? "BLE: skeleton 2 connected." : "BLE: skeleton 2 disconnected; reconnecting.");
  }
  Packet reply;
  while (SkeletonLink::receive(reply)) {
    if (state == IDLE || !SkeletonLink::sameRequest(reply, request) || reply.kind == SkeletonLink::PLAY) continue;
    if (lastStatus != reply.kind) {
      Serial.print("BLE: skeleton 2 "); Serial.print(SkeletonLink::kindName(reply.kind));
      Serial.print(' '); Serial.println(reply.name);
      lastStatus = reply.kind;
    }
    if (reply.kind == SkeletonLink::DONE || reply.kind == SkeletonLink::FAILED) {
      state = IDLE;
      SkeletonLink::cancelSend();
      Serial.println(reply.kind == SkeletonLink::DONE ? "Skeleton 1's turn is available again." : "Partner could not play the matching file. Check skeleton 2's Serial Monitor/card.");
    } else if (!acknowledged) {
      acknowledged = true;
      waitingSince = now;
    }
  }
  if (state != WAITING) return;
  const unsigned long timeout = acknowledged ? REPLY_TIMEOUT_MS : DELIVERY_TIMEOUT_MS;
  if (now - waitingSince >= timeout) {
    state = FAULT;
    SkeletonLink::cancelSend();
    Serial.println("BLE reply timed out. Motion is paused. Check skeleton 2, then press this button to retry the reply.");
    return;
  }
  if (connected && now - lastSend >= 1000 && SkeletonLink::send(request)) lastSend = now;
}
#else
static Preferences preferences;
static bool storageReady = false;
static Packet lastReply;
static bool haveLastReply = false;
static bool pending = false;
static bool working = false;
inline bool active() { return pending || working; }

inline bool saveReply() {
  uint8_t wire[SkeletonLink::WIRE_SIZE];
  SkeletonLink::encode(lastReply, wire);
  return storageReady && preferences.putBytes("last", wire, sizeof(wire)) == sizeof(wire);
}

inline void begin() {
  storageReady = preferences.begin("skeleton-reply", false);
  uint8_t wire[SkeletonLink::WIRE_SIZE];
  if (storageReady && preferences.getBytesLength("last") == sizeof(wire) &&
      preferences.getBytes("last", wire, sizeof(wire)) == sizeof(wire) &&
      SkeletonLink::decode(wire, sizeof(wire), lastReply)) {
    haveLastReply = true;
    if (lastReply.kind != SkeletonLink::DONE && lastReply.kind != SkeletonLink::FAILED) {
      // A reset interrupted this request. Do not replay it on retransmission.
      lastReply.kind = SkeletonLink::FAILED;
      saveReply();
    }
  }
  if (SkeletonLink::begin()) {
    Serial.println("BLE: skeleton 2 is advertising; waiting for skeleton 1.");
    if (haveLastReply) SkeletonLink::send(lastReply);
  } else Serial.println("BLE initialization failed. Restart this Arduino.");
}

inline bool allowButton(unsigned long) {
  if (!active()) return true;
  Serial.println("A matching reply is queued/playing. Button is available after this reply.");
  return false;
}

inline void playbackStarted() {
  if (!working) return;
  lastReply.kind = SkeletonLink::PLAYING;
  SkeletonLink::send(lastReply);
}

inline void finishRemote(bool success) {
  if (!active()) return;
  lastReply.kind = success ? SkeletonLink::DONE : SkeletonLink::FAILED;
  pending = false;
  working = false;
  saveReply();
  SkeletonLink::send(lastReply);
  Serial.print("BLE reply "); Serial.print(success ? "finished: " : "failed: "); Serial.println(lastReply.name);
}

inline void service(unsigned long now) {
  SkeletonLink::service();
  const bool connected = SkeletonLink::isConnected();
  if (connected != previousConnection) {
    previousConnection = connected;
    Serial.println(connected ? "BLE: skeleton 1 connected." : "BLE: skeleton 1 disconnected; advertising again.");
  }
  Packet command;
  while (SkeletonLink::receive(command)) {
    if (command.kind != SkeletonLink::PLAY) continue;
    if (haveLastReply && SkeletonLink::sameRequest(command, lastReply)) {
      SkeletonLink::send(lastReply);
      continue;
    }
    if (haveLastReply && command.session == lastReply.session && command.sequence <= lastReply.sequence) {
      command.kind = SkeletonLink::FAILED;
      SkeletonLink::send(command);
      continue;
    }
    if (active()) {
      command.kind = SkeletonLink::BUSY;
      SkeletonLink::send(command);
      continue;
    }
    lastReply = command;
    lastReply.kind = SkeletonLink::ACCEPTED;
    haveLastReply = true;
    if (!saveReply()) {
      lastReply.kind = SkeletonLink::FAILED;
      Serial.println("Cannot store BLE request identity; reply rejected to avoid duplicate playback.");
      SkeletonLink::send(lastReply);
      continue;
    }
    pending = true;
    SkeletonLink::send(lastReply);
    Serial.print("BLE: queued matching filename "); Serial.println(lastReply.name);
  }
  if (pending && !playerBusyForReply()) {
    pending = false;
    working = true;
    startRemoteReply(lastReply.name, now);
  }
}
#endif
}  // namespace SkeletonTurn
