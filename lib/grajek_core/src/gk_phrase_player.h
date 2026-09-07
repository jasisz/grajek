// Bounded polyphonic phrase playback. No allocation or hardware clock.
#pragma once

#include "gk_garden.h"

namespace gk {

struct PhraseEvent {
  bool down = false;
  int slot = 0;  // stable for the entire phrase, never reused by a pending off
  GardenPhraseNote note;
};

struct PhraseEvents {
  PhraseEvent events[2 * kGardenPhraseMax];
  int count = 0;
};

class PhrasePlayer {
 public:
  // Call cancel() and deliver its offs before replacing an active phrase.
  bool start(const GardenPhrase& phrase, uint32_t nowMs);
  PhraseEvents tick(uint32_t nowMs);
  PhraseEvents cancel();
  bool active() const { return next_ < phrase_.count || sounding_ != 0; }

 private:
  GardenPhrase phrase_;
  uint32_t onset_[kGardenPhraseMax]{};
  uint32_t startMs_ = 0;
  uint32_t sounding_ = 0;
  int next_ = 0;
};

}  // namespace gk
