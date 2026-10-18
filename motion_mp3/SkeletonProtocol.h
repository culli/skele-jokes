#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>

// Identical copies live beside each sketch so either folder is self-contained.
namespace SkeletonLink {
constexpr size_t WIRE_SIZE = 20;  // Fits the default BLE ATT payload.
constexpr size_t NAME_SIZE = 13;  // Eight-character base + dot + extension + NUL.
enum Kind : uint8_t { PLAY = 1, ACCEPTED, PLAYING, DONE, FAILED, BUSY };
struct Packet {
  Kind kind = PLAY;
  uint32_t session = 0;
  uint16_t sequence = 0;
  char name[NAME_SIZE] = {};
};

inline bool normalizeName(const char* input, char* output) {
  size_t length = 0;
  while (length < NAME_SIZE && input[length]) ++length;
  if (length < 5 || length >= NAME_SIZE || input[0] == '_' || input[0] == '.') return false;
  const size_t dot = length - 4;
  if (dot < 1 || dot > 8 || input[dot] != '.') return false;
  for (size_t i = 0; i < length; ++i) {
    char c = input[i];
    if (c >= 'a' && c <= 'z') c -= 'a' - 'A';
    if (i != dot && !((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '~')) return false;
    output[i] = c;
  }
  output[length] = 0;
  return strcmp(output + dot, ".MP3") == 0 || strcmp(output + dot, ".WAV") == 0;
}

inline bool sameRequest(const Packet& a, const Packet& b) {
  return a.session == b.session && a.sequence == b.sequence && strcmp(a.name, b.name) == 0;
}

inline void encode(const Packet& packet, uint8_t* wire) {
  memset(wire, 0, WIRE_SIZE);
  wire[0] = 1;
  wire[1] = packet.kind;
  for (size_t i = 0; i < 4; ++i) wire[2 + i] = packet.session >> (8 * i);
  wire[6] = packet.sequence & 255;
  wire[7] = packet.sequence >> 8;
  for (size_t i = 0; i < NAME_SIZE - 1 && packet.name[i]; ++i) wire[8 + i] = packet.name[i];
}

inline bool decode(const uint8_t* wire, size_t length, Packet& packet) {
  if (length != WIRE_SIZE || wire[0] != 1 || wire[1] < PLAY || wire[1] > BUSY) return false;
  Packet result;
  result.kind = static_cast<Kind>(wire[1]);
  for (size_t i = 0; i < 4; ++i) result.session |= static_cast<uint32_t>(wire[2 + i]) << (8 * i);
  result.sequence = static_cast<uint16_t>(wire[6]) | (static_cast<uint16_t>(wire[7]) << 8);
  if (!result.session || !result.sequence) return false;
  char raw[NAME_SIZE] = {};
  memcpy(raw, wire + 8, NAME_SIZE - 1);
  bool ended = false;
  for (size_t i = 0; i < NAME_SIZE - 1; ++i) {
    if (!raw[i]) ended = true;
    else if (ended) return false;
  }
  if (!normalizeName(raw, result.name)) return false;
  packet = result;
  return true;
}

inline const char* kindName(Kind kind) {
  switch (kind) {
    case PLAY: return "request";
    case ACCEPTED: return "accepted";
    case PLAYING: return "playing";
    case DONE: return "finished";
    case FAILED: return "failed";
    case BUSY: return "busy";
  }
  return "unknown";
}
}  // namespace SkeletonLink
