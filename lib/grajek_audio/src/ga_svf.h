// State-variable filter (trapezoidal topology after A. Simper / Cytomic) —
// stable across the whole sweep range, cheap: tanf only on parameter change.
#pragma once
#include <math.h>
#include "ga_dsp.h"

namespace ga {

// The trapezoidal state-variable topology computes all four responses from
// the same two states.  Keeping the selector here (rather than building four
// filters) makes extra timbres effectively free.
enum class FilterMode : uint8_t { Lowpass = 0, Highpass, Bandpass, Notch };

struct SVFFrame {
  float low = 0.0f;
  float high = 0.0f;
  float band = 0.0f;
  float notch = 0.0f;

  float select(FilterMode mode) const {
    switch (mode) {
      case FilterMode::Highpass: return high;
      case FilterMode::Bandpass: return band;
      case FilterMode::Notch:    return notch;
      case FilterMode::Lowpass:
      default:                   return low;
    }
  }
};

class SVF {
 public:
  void init(float sampleRate) {
    sr_ = sampleRate;
    set(8000.0f, 0.1f);
    reset();
  }
  void reset() { ic1_ = ic2_ = 0.0f; }

  void set(float cutoffHz, float res01) {
    cutoffHz = clampf(cutoffHz, 20.0f, sr_ * 0.45f);
    res01 = clampf(res01, 0.0f, 0.98f);
    g_ = tanf(kPi * cutoffHz / sr_);
    k_ = 2.0f - 1.9f * res01;  // res 0 -> k=2 (no resonance), res 1 -> k~0.1
    a1_ = 1.0f / (1.0f + g_ * (g_ + k_));
    a2_ = g_ * a1_;
    a3_ = g_ * a2_;
  }

  // Call once per block: zeroes state below the subnormal threshold so the
  // silent decay tail never enters denormal range (10-100x FP slowdown on
  // some hosts).
  void flushDenormals() {
    if (fabsf(ic1_) < 1e-15f) ic1_ = 0.0f;
    if (fabsf(ic2_) < 1e-15f) ic2_ = 0.0f;
  }

  SVFFrame process(float v0) {
    const float v3 = v0 - ic2_;
    const float v1 = a1_ * ic1_ + a2_ * v3;
    const float v2 = ic2_ + a2_ * ic1_ + a3_ * v3;
    ic1_ = 2.0f * v1 - ic1_;
    ic2_ = 2.0f * v2 - ic2_;
    // TPT SVF identities: v2=LP, v1=BP, HP=x-k*BP-LP,
    // notch=HP+LP.  No extra filter state or trigonometry is required.
    const float high = v0 - k_ * v1 - v2;
    return {v2, high, v1, high + v2};
  }

  // Lowpass-only fast path. Every preset that is not mid-crossfade uses it,
  // so it deliberately skips the high/notch algebra: on the ESP32 those three
  // extra operations are paid 48000 times a second for nothing.
  float processLP(float v0) {
    const float v3 = v0 - ic2_;
    const float v1 = a1_ * ic1_ + a2_ * v3;
    const float v2 = ic2_ + a2_ * ic1_ + a3_ * v3;
    ic1_ = 2.0f * v1 - ic1_;
    ic2_ = 2.0f * v2 - ic2_;
    return v2;
  }

 private:
  float sr_ = 48000.0f;
  float g_ = 0.0f, k_ = 2.0f, a1_ = 0.0f, a2_ = 0.0f, a3_ = 0.0f;
  float ic1_ = 0.0f, ic2_ = 0.0f;
};

}  // namespace ga
