#include "gk_phrase_player.h"

namespace gk {

bool PhrasePlayer::start(const GardenPhrase& phrase, uint32_t nowMs) {
  if (active() || phrase.count <= 0 || phrase.count > kGardenPhraseMax)
    return false;
  phrase_ = phrase;
  startMs_ = nowMs;
  next_ = 0;
  sounding_ = 0;
  uint32_t onset = 0;
  for (int i = 0; i < phrase_.count; ++i) {
    if (i > 0) onset += Garden::replayGapMs(phrase_.note[i].gapMs);
    onset_[i] = onset;
    phrase_.note[i].holdMs = Garden::replayHoldMs(phrase_.note[i].holdMs);
  }
  return true;
}

PhraseEvents PhrasePlayer::tick(uint32_t nowMs) {
  PhraseEvents out;
  const uint32_t elapsed = nowMs - startMs_;
  // Existing releases precede new attacks, leaving room for the foreground.
  for (int i = 0; i < next_; ++i) {
    if ((sounding_ & (1u << i)) &&
        elapsed >= onset_[i] + phrase_.note[i].holdMs) {
      out.events[out.count++] = {false, i, phrase_.note[i]};
      sounding_ &= ~(1u << i);
    }
  }
  // Drain the entire chord in this call. Absolute offsets avoid tempo drift
  // from one late UI frame being added to every subsequent note.
  while (next_ < phrase_.count && elapsed >= onset_[next_]) {
    const int i = next_++;
    out.events[out.count++] = {true, i, phrase_.note[i]};
    sounding_ |= 1u << i;
    if (elapsed >= onset_[i] + phrase_.note[i].holdMs) {
      out.events[out.count++] = {false, i, phrase_.note[i]};
      sounding_ &= ~(1u << i);
    }
  }
  return out;
}

PhraseEvents PhrasePlayer::cancel() {
  PhraseEvents out;
  for (int i = 0; i < phrase_.count; ++i)
    if (sounding_ & (1u << i))
      out.events[out.count++] = {false, i, phrase_.note[i]};
  sounding_ = 0;
  next_ = phrase_.count;
  return out;
}

}  // namespace gk
