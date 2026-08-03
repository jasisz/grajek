#include "gk_lullaby.h"

#include <math.h>

namespace gk {

bool LullabySequencer::due(uint32_t nowMs, uint32_t deadlineMs) {
  return static_cast<int32_t>(nowMs - deadlineMs) >= 0;
}

bool LullabySequencer::start(uint32_t nowMs, bool playedThisSession,
                             const Garden& garden) {
  if (state_ != LullabyState::None || !playedThisSession ||
      garden.count() == 0)
    return false;
  state_ = LullabyState::Singing;
  index_ = 0;
  slowdown_ = 1.0f;
  velocity_ = 0.18f;
  nextMs_ = nowMs + 1200;
  saveAtMs_ = 0;
  savePending_ = false;
  return true;
}

void LullabySequencer::abort() {
  state_ = LullabyState::None;
  saveAtMs_ = 0;
  savePending_ = false;
}

LullabyEvent LullabySequencer::tick(uint32_t nowMs, const Garden& garden) {
  LullabyEvent event;
  if (state_ == LullabyState::Sleeping) {
    if (savePending_ && due(nowMs, saveAtMs_))
      event.type = LullabyEventType::SaveDue;
    return event;
  }
  if (state_ != LullabyState::Singing || !due(nowMs, nextMs_)) return event;

  if (index_ >= garden.count()) {
    state_ = LullabyState::Sleeping;
    saveAtMs_ = nowMs + 3000;
    savePending_ = true;
    event.type = LullabyEventType::EnterSleep;
    return event;
  }

  const bool more = index_ + 1 < garden.count();
  uint32_t gap = 1400;
  if (more)
    gap = garden.startsPhrase(index_ + 1)
              ? 1400
              : Garden::replayGapMs(garden.delayMs(index_ + 1));
  gap = static_cast<uint32_t>(static_cast<float>(gap) * slowdown_);

  event.type = LullabyEventType::PlayNote;
  event.cents = garden.cents(index_);
  event.velocity = velocity_;
  ++index_;
  event.progress = static_cast<float>(index_) /
                   static_cast<float>(garden.count() > 1 ? garden.count() : 1);

  nextMs_ = nowMs + gap;
  slowdown_ = fminf(1.8f, slowdown_ * 1.06f);
  velocity_ = fmaxf(0.045f, velocity_ * 0.88f);
  return event;
}

void LullabySequencer::acknowledgeSave(bool success, uint32_t nowMs) {
  if (state_ != LullabyState::Sleeping || !savePending_) return;
  if (success) {
    savePending_ = false;
  } else {
    saveAtMs_ = nowMs + 30000;
  }
}

}  // namespace gk
