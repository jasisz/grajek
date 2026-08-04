#pragma once

#include <stdint.h>

// Small, hardware-independent timer for the second way into the goodnight
// ritual. Turning the box face down is the deliberate way; this is the one
// for a child who simply walked off. After a long enough silence from the
// PLAYER the instrument performs exactly the same ritual: lullaby, screen
// off, and the garden singing itself to sleep.
//
// The box humming to itself must never count as activity — ghosts, weather
// and the heartbeat all play on their own, and an instrument that kept itself
// awake by breathing would never sleep at all. Only a human touching a key,
// the side button, a deliberate swing, or lifting the box out of face-down
// restarts this clock.
class IdleSleep {
 public:
  // Deliberately short: the box should go quiet soon after the child wanders
  // off, not keep the room company for a quarter of an hour. Falling asleep
  // early costs almost nothing — any key wakes it instantly — while a box
  // that drones on long after everyone left is the thing people unplug.
  static constexpr uint32_t kIdleMs = 3u * 60u * 1000u;  // 3 minutes

  void noteActivity(uint32_t nowMs) {
    lastMs_ = nowMs;
    seeded_ = true;
    spent_ = false;
  }

  // Returns true on the single pass where the silence has lasted long enough.
  // `sleeping` covers the box already being asleep for any reason: the timer
  // then stays spent until real activity rearms it, so a lullaby that ends by
  // itself is not immediately followed by another one.
  bool update(uint32_t nowMs, bool sleeping) {
    if (!seeded_) {  // a box switched on and never touched still falls asleep
      lastMs_ = nowMs;
      seeded_ = true;
    }
    if (sleeping) {
      spent_ = true;
      return false;
    }
    if (spent_) return false;
    // Unsigned arithmetic, so this stays correct across the millis() wrap.
    if (nowMs - lastMs_ < kIdleMs) return false;
    spent_ = true;
    return true;
  }

 private:
  uint32_t lastMs_ = 0;
  bool seeded_ = false;
  bool spent_ = false;
};
