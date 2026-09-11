// GO receives debounced edges. A hold fires once and consumes its release,
// even if that hold changed the screen before the release arrived.
#pragma once
#include <stdint.h>

namespace gk {

enum class GoAction { None, Wake, NextWorld, OpenSettings, Play };

class GoButton {
 public:
  static constexpr uint32_t kHoldMs = 700;

  GoAction update(uint32_t nowMs, bool pressed, bool released,
                  bool inSettings, bool sleeping) {
    if (pressed) {
      down_ = true;
      used_ = false;
      wakeOnly_ = sleeping;
      settingsAtPress_ = inSettings;
      startedMs_ = nowMs;
      if (wakeOnly_) return GoAction::Wake;
    }
    if (!down_) return GoAction::None;
    const bool held = nowMs - startedMs_ >= kHoldMs;
    GoAction action = GoAction::None;
    if (!used_ && !wakeOnly_ && (held || released)) {
      used_ = true;
      action = settingsAtPress_ ? GoAction::Play :
               (held ? GoAction::OpenSettings : GoAction::NextWorld);
    }
    if (released) down_ = false;
    return action;
  }

 private:
  uint32_t startedMs_ = 0;
  bool down_ = false;
  bool used_ = false;
  bool wakeOnly_ = false;
  bool settingsAtPress_ = false;
};

}  // namespace gk
