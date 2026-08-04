// Lo-fi bridge: runs a sample-hungry effect at 1/Div of the audio rate to fit
// long tape buffers into small RAM. Decimates by averaging each group of Div
// samples, lets the wrapped effect produce its wet signal at the low rate,
// then linearly interpolates the wet back up and adds it to the full-rate
// buffer. The aging-tape aesthetic loves this: repeats are supposed to come
// back darker anyway.
//
// The divisor is chosen so the wrapped effect lands on ~16 kHz: the tape's
// aging filters are tuned around 4.5 kHz and need real headroom above them.
// Div=3 suited a 48 kHz engine; at 32 kHz that would drop the tape to 10.7 kHz,
// putting the darkening filter right under Nyquist, where it stops darkening.
//
// The wrapped effect must follow the EchoTape convention:
//   process(in, out, n) writes out[i] = dry + wet (in/out may alias).
#pragma once

namespace ga {

template <typename Fx, int Div>
class LofiBridge {
 public:
  static_assert(Div >= 2, "a lo-fi bridge must actually decimate");
  static constexpr int kDiv = Div;
  static constexpr int kMaxLow = 96;  // supports blocks up to Div * 96 frames

  void init(Fx* fx) {
    fx_ = fx;
    prevWet_ = 0.0f;
  }

  // n must be divisible by Div and <= Div * kMaxLow
  void process(float* buf, int n) {
    if (!fx_) return;
    const int m = n / Div;
    float low[kMaxLow];
    float wet[kMaxLow];
    for (int j = 0; j < m; ++j) {
      float sum = 0.0f;
      for (int d = 0; d < Div; ++d) sum += buf[Div * j + d];
      low[j] = sum * (1.0f / (float)Div);
    }
    for (int j = 0; j < m; ++j) wet[j] = low[j];
    fx_->process(low, wet, m);            // wet[] = dry + effect
    for (int j = 0; j < m; ++j) wet[j] -= low[j];  // isolate the effect
    float a = prevWet_;
    for (int j = 0; j < m; ++j) {
      const float b = wet[j];
      for (int d = 0; d < Div; ++d)
        buf[Div * j + d] += a + (b - a) * ((float)(d + 1) / (float)Div);
      a = b;
    }
    prevWet_ = a;
  }

 private:
  Fx* fx_ = nullptr;
  float prevWet_ = 0.0f;
};

template <typename Fx>
using LofiBridge3 = LofiBridge<Fx, 3>;

}  // namespace ga
