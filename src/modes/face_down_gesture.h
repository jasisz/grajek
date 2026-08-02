#pragma once

#include <stdint.h>

// Small, hardware-independent state machine for the goodnight gesture.
// Orientation itself is the dwell condition: the shake envelope cannot be
// used here because it is measured against a slow gravity filter, so turning
// the box over looks like motion until that filter catches up.
class FaceDownGesture {
 public:
  enum class Event : uint8_t { None, Sleep, Wake };

  static constexpr float kEnterZ = -0.80f;
  static constexpr float kLeaveZ = -0.55f;
  static constexpr uint32_t kDwellMs = 1200;

  Event update(uint32_t nowMs, float accelZ, bool sleeping) {
    if (accelZ < kEnterZ) {
      if (!candidate_) {
        candidate_ = true;
        requested_ = false;
        sinceMs_ = nowMs;
      } else if (!requested_ && !sleeping &&
                 nowMs - sinceMs_ >= kDwellMs) {
        requested_ = true;
        return Event::Sleep;
      }
    } else if (accelZ > kLeaveZ) {
      candidate_ = false;
      requested_ = false;
      if (sleeping) return Event::Wake;
    }
    return Event::None;
  }

  // A real key is an explicit wake-up. If the box is still face-down, a
  // complete new dwell is required before goodnight may start again.
  void restartDwell() {
    candidate_ = false;
    requested_ = false;
  }

 private:
  uint32_t sinceMs_ = 0;
  bool candidate_ = false;
  bool requested_ = false;
};
