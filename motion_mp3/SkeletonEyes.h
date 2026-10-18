#pragma once
#include <Arduino.h>
#include <esp_system.h>

// Active-LOW direct drive: 3V3 through 1k to an existing parallel LED pair's
// red wire, black wire to D6. This limits TOTAL current; brightness may differ.
// Alternatively use a separate 1k resistor for each LED, both cathodes at D6.
// For PNP: emitter on 3V3, base resistor to D6, base pull-up to 3V3.
// Direct GPIO drive is only for these low-current circuits in SKELETON-EYES.txt.
// Do not connect a 5V-powered PNP base directly to a Nano ESP32 GPIO.
namespace SkeletonEyes {
constexpr int PIN = D6;
constexpr bool ACTIVE_LOW = true;
enum Effect { STEADY, BLINK, FLICKER };
constexpr Effect EFFECT = FLICKER;
// Allow the receiver's bounded index/name retries, then go dark if UART
// playback status disappears. A reported stop/skip turns the eyes off at once.
constexpr uint32_t STATUS_TIMEOUT_MS = 8000;

static bool talking = false;
static bool lit = false;
static uint32_t changedAt = 0;
static uint32_t intervalMs = 0;
static uint32_t randomState = 1;

inline void drive(bool on) {
  lit = on;
  digitalWrite(PIN, (on != ACTIVE_LOW) ? HIGH : LOW);
}

inline uint32_t interval(bool on) {
  if (EFFECT == BLINK) return 250;
  randomState ^= randomState << 13;
  randomState ^= randomState >> 17;
  randomState ^= randomState << 5;
  return on ? 140 + randomState % 361 : 35 + randomState % 66;
}

inline void begin() {
  // Preload OFF before enabling output. An external pull-up holds D6 (or the
  // PNP base) high while the Arduino is resetting or booting.
  drive(false);
  pinMode(PIN, OUTPUT);
  drive(false);
  talking = false;
  randomState = esp_random();
  if (!randomState) randomState = 1;
}

inline void service(bool playbackConfirmed, uint32_t now) {
  if (!playbackConfirmed) {
    if (talking || lit) drive(false);
    talking = false;
    return;
  }
  if (!talking) {
    talking = true;
    drive(true);
    changedAt = now;
    intervalMs = interval(true);
  } else if (EFFECT != STEADY && static_cast<uint32_t>(now - changedAt) >= intervalMs) {
    drive(!lit);
    changedAt = now;
    intervalMs = interval(lit);
  }
}
}  // namespace SkeletonEyes
