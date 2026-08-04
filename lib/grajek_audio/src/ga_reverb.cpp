#include "ga_reverb.h"

#include <math.h>
#include <string.h>

namespace ga {

constexpr int Reverb::kCombRef[];
constexpr int Reverb::kAllpassRef[];

void Reverb::init(float sampleRate) {
  kHpWet_ = 1.0f - expf(-6.2831853f * 200.0f / sampleRate);
  const float scale = sampleRate / kRefRate;
  for (int c = 0; c < kNumCombs; ++c) {
    int len = (int)lroundf((float)kCombRef[c] * scale);
    if (len < 4) len = 4;
    if (len > kCombRef[c]) len = kCombRef[c];  // buffers are sized for kRefRate
    combLen_[c] = len;
  }
  for (int a = 0; a < kNumAllpass; ++a) {
    int len = (int)lroundf((float)kAllpassRef[a] * scale);
    if (len < 4) len = 4;
    if (len > kAllpassRef[a]) len = kAllpassRef[a];
    allpassLen_[a] = len;
  }
  hpLp_ = 0.0f;
  memset(comb_, 0, sizeof(comb_));
  memset(allpass_, 0, sizeof(allpass_));
  memset(combFilt_, 0, sizeof(combFilt_));
  int off = 0;
  for (int c = 0; c < kNumCombs; ++c) {
    combOff_[c] = off;
    combIdx_[c] = 0;
    off += kCombRef[c];
  }
  off = 0;
  for (int a = 0; a < kNumAllpass; ++a) {
    allpassOff_[a] = off;
    allpassIdx_[a] = 0;
    off += kAllpassRef[a];
  }
}

void Reverb::setWet(float w) {
  if (w < 0.0f) w = 0.0f;
  if (w > 1.0f) w = 1.0f;
  wet_.store(w, std::memory_order_relaxed);
}

void Reverb::setRoom(float r) {
  if (r < 0.0f) r = 0.0f;
  if (r > 1.0f) r = 1.0f;
  room_.store(0.7f + 0.28f * r, std::memory_order_relaxed);
}

void Reverb::setDamp(float d) {
  if (d < 0.0f) d = 0.0f;
  if (d > 1.0f) d = 1.0f;
  damp_.store(d, std::memory_order_relaxed);
}

void Reverb::process(float* buf, int n) {
  const float wet = wet_.load(std::memory_order_relaxed);
  if (wet <= 0.0f) return;
  const float fb = room_.load(std::memory_order_relaxed);
  const float damp = damp_.load(std::memory_order_relaxed);

  // Denormal flush at block rate, not per comb per sample. The comb loop
  // decays by ~0.99996 a sample, so reaching the subnormal range takes on the
  // order of a million samples — a per-block flush keeps a safety factor of
  // thousands while removing a two-sided compare from the innermost loop.
  for (int c = 0; c < kNumCombs; ++c)
    if (combFilt_[c] < 1e-18f && combFilt_[c] > -1e-18f) combFilt_[c] = 0.0f;
  if (hpLp_ < 1e-18f && hpLp_ > -1e-18f) hpLp_ = 0.0f;

  for (int i = 0; i < n; ++i) {
    const float input = buf[i] * 0.015f;  // classic Freeverb input scaling
    float s = 0.0f;
    for (int c = 0; c < kNumCombs; ++c) {
      float* b = comb_ + combOff_[c];
      int& idx = combIdx_[c];
      const float out = b[idx];
      combFilt_[c] = out * (1.0f - damp) + combFilt_[c] * damp;
      b[idx] = input + combFilt_[c] * fb;
      if (++idx >= combLen_[c]) idx = 0;
      s += out;
    }
    for (int a = 0; a < kNumAllpass; ++a) {
      float* b = allpass_ + allpassOff_[a];
      int& idx = allpassIdx_[a];
      const float bufout = b[idx];
      const float out = -s + bufout;
      b[idx] = s + bufout * 0.5f;
      if (++idx >= allpassLen_[a]) idx = 0;
      s = out;
    }
    // high-pass the wet sum (~200 Hz): the tail adds air, never mud
    hpLp_ += kHpWet_ * (s - hpLp_);
    s -= hpLp_;
    buf[i] += s * wet * 3.0f;  // wet make-up gain (Freeverb convention)
  }
}

}  // namespace ga
