// Goodnight phrase sequencer. Hardware and sound actions are returned as
// events so the state machine can be tested without Arduino or an audio device.
#pragma once

#include <stdint.h>

#include "gk_garden.h"

namespace gk {

enum class LullabyState : uint8_t { None, Singing, Sleeping };
enum class LullabyEventType : uint8_t { None, PlayNote, EnterSleep, SaveDue };

struct LullabyEvent {
  LullabyEventType type = LullabyEventType::None;
  float cents = 0.0f;
  float velocity = 0.0f;
  float progress = 0.0f;
};

class LullabySequencer {
 public:
  bool start(uint32_t nowMs, bool playedThisSession, const Garden& garden);
  void abort();
  LullabyEvent tick(uint32_t nowMs, const Garden& garden);
  void acknowledgeSave(bool success, uint32_t nowMs);

  LullabyState state() const { return state_; }
  bool active() const { return state_ != LullabyState::None; }

 private:
  static bool due(uint32_t nowMs, uint32_t deadlineMs);

  LullabyState state_ = LullabyState::None;
  uint32_t nextMs_ = 0;
  uint32_t saveAtMs_ = 0;
  bool savePending_ = false;
  float slowdown_ = 1.0f;
  float velocity_ = 0.0f;
  int index_ = 0;
};

}  // namespace gk
