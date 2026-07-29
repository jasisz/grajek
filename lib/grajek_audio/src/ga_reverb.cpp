#include "ga_reverb.h"

#include <string.h>

namespace ga {

constexpr int Reverb::kCombLen[];
constexpr int Reverb::kAllpassLen[];

void Reverb::init(float sampleRate) {
  (void)sampleRate;  // lengths are tuned for 48 kHz
  memset(comb_, 0, sizeof(comb_));
  memset(allpass_, 0, sizeof(allpass_));
  memset(combFilt_, 0, sizeof(combFilt_));
  int off = 0;
  for (int c = 0; c < kNumCombs; ++c) {
    combOff_[c] = off;
    combIdx_[c] = 0;
    off += kCombLen[c];
  }
  off = 0;
  for (int a = 0; a < kNumAllpass; ++a) {
    allpassOff_[a] = off;
    allpassIdx_[a] = 0;
    off += kAllpassLen[a];
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

  for (int i = 0; i < n; ++i) {
    const float input = buf[i] * 0.015f;  // classic Freeverb input scaling
    float s = 0.0f;
    for (int c = 0; c < kNumCombs; ++c) {
      float* b = comb_ + combOff_[c];
      int& idx = combIdx_[c];
      const float out = b[idx];
      combFilt_[c] = out * (1.0f - damp) + combFilt_[c] * damp;
      // flush denormals in the tail
      if (combFilt_[c] < 1e-18f && combFilt_[c] > -1e-18f) combFilt_[c] = 0.0f;
      b[idx] = input + combFilt_[c] * fb;
      if (++idx >= kCombLen[c]) idx = 0;
      s += out;
    }
    for (int a = 0; a < kNumAllpass; ++a) {
      float* b = allpass_ + allpassOff_[a];
      int& idx = allpassIdx_[a];
      const float bufout = b[idx];
      const float out = -s + bufout;
      b[idx] = s + bufout * 0.5f;
      if (++idx >= kAllpassLen[a]) idx = 0;
      s = out;
    }
    buf[i] += s * wet * 3.0f;  // wet make-up gain (Freeverb convention)
  }
}

}  // namespace ga
