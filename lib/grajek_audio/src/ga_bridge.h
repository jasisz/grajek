// Lo-fi bridge: runs a sample-hungry effect at 1/3 of the audio rate
// (48 kHz -> 16 kHz) to fit long tape buffers into small RAM.
// Decimates by averaging triplets, lets the wrapped effect produce its wet
// signal at the low rate, then linearly interpolates the wet back up and
// adds it to the full-rate buffer. The aging-tape aesthetic loves this:
// repeats are supposed to come back darker anyway.
//
// The wrapped effect must follow the EchoTape convention:
//   process(in, out, n) writes out[i] = dry + wet (in/out may alias).
#pragma once

namespace ga {

template <typename Fx>
class LofiBridge3 {
 public:
  static constexpr int kMaxLow = 64;  // supports blocks up to 192 frames

  void init(Fx* fx) {
    fx_ = fx;
    prevWet_ = 0.0f;
  }

  // n must be divisible by 3 and <= 3 * kMaxLow
  void process(float* buf, int n) {
    if (!fx_) return;
    const int m = n / 3;
    float low[kMaxLow];
    float wet[kMaxLow];
    for (int j = 0; j < m; ++j)
      low[j] = (buf[3 * j] + buf[3 * j + 1] + buf[3 * j + 2]) *
               (1.0f / 3.0f);
    for (int j = 0; j < m; ++j) wet[j] = low[j];
    fx_->process(low, wet, m);            // wet[] = dry + effect
    for (int j = 0; j < m; ++j) wet[j] -= low[j];  // isolate the effect
    float a = prevWet_;
    for (int j = 0; j < m; ++j) {
      const float b = wet[j];
      buf[3 * j] += a + (b - a) * (1.0f / 3.0f);
      buf[3 * j + 1] += a + (b - a) * (2.0f / 3.0f);
      buf[3 * j + 2] += b;
      a = b;
    }
    prevWet_ = a;
  }

 private:
  Fx* fx_ = nullptr;
  float prevWet_ = 0.0f;
};

}  // namespace ga
