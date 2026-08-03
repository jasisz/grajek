#include "gk_pulse.h"

#include <math.h>
#include <string.h>

namespace gk {

void PulseTracker::onOnset(double nowSec) {
  const double distance = lastOnset_ > 0.0 ? nowSec - lastOnset_ : 0.0;
  if (count_ < 8) {
    onsets_[count_++] = nowSec;
  } else {
    memmove(onsets_, onsets_ + 1, 7 * sizeof(double));
    onsets_[7] = nowSec;
  }
  lastOnset_ = nowSec;

  if (ticking_) {
    beatsAlone_ = 0;
    const double ratio = period_ > 0.0 ? distance / period_ : 0.0;
    if (ratio > 0.7 && ratio < 1.4) {
      period_ += 0.25 * (distance - period_);
    } else if (ratio > 1.6 && ratio < 2.4) {
      period_ += 0.12 * (distance * 0.5 - period_);
    } else if (ratio > 0.35 && ratio < 0.65) {
      period_ += 0.12 * (distance * 2.0 - period_);
    }
    memory_ = period_;
    double error = nowSec - nextBeatAt_;
    error -= period_ * round(error / period_);
    nextBeatAt_ += 0.5 * error;
    while (nextBeatAt_ <= nowSec) nextBeatAt_ += period_;
    return;
  }

  double intervals[7];
  int intervalCount = 0;
  for (int i = 1; i < count_; ++i) {
    const double interval = onsets_[i] - onsets_[i - 1];
    if (interval > 0.18 && interval < 2.0)
      intervals[intervalCount++] = interval;
  }
  if (intervalCount < 3) return;

  const int windowCount = intervalCount < 5 ? intervalCount : 5;
  double window[5];
  for (int i = 0; i < windowCount; ++i)
    window[i] = intervals[intervalCount - windowCount + i];
  for (int i = 1; i < windowCount; ++i) {
    const double value = window[i];
    int j = i;
    while (j > 0 && window[j - 1] > value) {
      window[j] = window[j - 1];
      --j;
    }
    window[j] = value;
  }
  const double median = window[windowCount / 2];
  int regular = 0;
  for (int i = 0; i < windowCount; ++i)
    if (fabs(window[i] - median) < 0.15 * median) ++regular;
  if (regular >= windowCount - 1) {
    period_ = median;
    memory_ = median;
    nextBeatAt_ = nowSec + period_;
    beatsAlone_ = 0;
    beatsPlayed_ = 0;
    step_ = 0;
    ticking_ = true;
  }
}

PulseBeat PulseTracker::tick(double nowSec, int chordNoteCount) {
  PulseBeat beat;
  if (!ticking_ || nowSec < nextBeatAt_) return beat;

  if (nowSec - lastOnset_ > period_ * 1.5) ++beatsAlone_;
  const float fade = 1.0f - static_cast<float>(beatsAlone_) / 6.0f;
  if (fade <= 0.0f) {
    suspend();
    return beat;
  }
  if (beatsPlayed_ < 12) ++beatsPlayed_;
  const float warm = beatsPlayed_ < 3
                         ? 0.3f + static_cast<float>(beatsPlayed_) * 0.23f
                         : 1.0f;

  float accent = 1.0f;
  if (chordNoteCount > 0) {
    beat.chordIndex = step_ % chordNoteCount;
    accent = beat.chordIndex == 0 ? 1.2f : 0.8f;
    ++step_;
  }
  beat.velocity = 0.17f * fade * warm * accent;
  if (beat.velocity > 0.24f) beat.velocity = 0.24f;
  beat.play = true;

  nextBeatAt_ += period_;
  while (nextBeatAt_ < nowSec) nextBeatAt_ += period_;
  return beat;
}

bool PulseTracker::suspend() {
  const bool wasTicking = ticking_;
  ticking_ = false;
  count_ = 0;
  beatsAlone_ = 0;
  beatsPlayed_ = 0;
  step_ = 0;
  return wasTicking;
}

}  // namespace gk
