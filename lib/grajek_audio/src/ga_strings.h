// Sympathetic strings in just intonation (a "jawari box"), header-only.
//
// Five damped comb resonators (Karplus strings without noise excitation)
// tuned to pure ratios of the current root: 1/1, 3/2, 2/1, 9/4, 3/1.
// They never play by themselves — everything the engine plays excites them
// lightly, so every note leaves a halo strictly in consonant intervals.
// This is the only element of the chain that adds a NEW spectrum (resonant
// string-like sustain instead of sine partials).
//
// Retuning (setRootHz) slews the delay lengths over ~100 ms — a toss landing
// bends the whole halo onto the new tonal center instead of clicking.
//
// State: 5 x 896 int16 samples ≈ 9 KB — fits the ESP32-S3 without PSRAM.
// Threading: setters atomic (control thread), process() audio thread.
#pragma once
#include <stdint.h>
#include <math.h>
#include <string.h>
#include <atomic>

namespace ga {

class SympatheticStrings {
 public:
  static constexpr int kNumStrings = 5;

  void init(float sampleRate) {
    sr_ = sampleRate;
    for (int s = 0; s < kNumStrings; ++s) {
      memset(buf_[s], 0, sizeof(buf_[s]));
      len_[s] = lenFor(220.0f, s);
      damp_[s] = dcX_[s] = dcY_[s] = 0.0f;
      w_[s] = 0;
    }
    root_.store(220.0f);
    enabled_.store(true);
  }

  // --- control thread ---
  void setRootHz(float hz) {
    if (hz < 55.0f) hz = 55.0f;      // the 1/1 string buffer bottoms out here
    if (hz > 1500.0f) hz = 1500.0f;
    root_.store(hz, std::memory_order_relaxed);
  }
  void setEnabled(bool on) { enabled_.store(on, std::memory_order_relaxed); }
  bool enabled() const { return enabled_.load(std::memory_order_relaxed); }

  // --- audio thread: `in` excites the strings, their ring is ADDED to out.
  // in/out may alias (in[i] is read before out[i] is written). ---
  void process(const float* in, float* out, int n) {
    if (!enabled_.load(std::memory_order_relaxed)) return;
    const float root = root_.load(std::memory_order_relaxed);

    // slewed retune: ~0.05 samples of length per sample = a ~120 ms tape
    // bend onto the new center instead of a click
    const float maxStep = 0.05f * (float)n;
    for (int s = 0; s < kNumStrings; ++s) {
      const float target = lenFor(root, s);
      float d = target - len_[s];
      if (d > maxStep) d = maxStep;
      if (d < -maxStep) d = -maxStep;
      len_[s] += d;
    }

    for (int i = 0; i < n; ++i) {
      const float x = in[i] * kExcite;
      float sum = 0.0f;
      for (int s = 0; s < kNumStrings; ++s) {
        float rp = (float)w_[s] - len_[s];
        if (rp < 0.0f) rp += (float)kMaxLen;
        const int i0 = (int)rp;
        int i1 = i0 + 1;
        if (i1 >= kMaxLen) i1 = 0;
        const float frac = rp - (float)i0;
        const float rd = ((float)buf_[s][i0] * (1.0f - frac) +
                          (float)buf_[s][i1] * frac) *
                         (1.0f / 32768.0f);
        // gentle damping keeps the ring warm, the DC blocker keeps the
        // near-unity feedback loop honest
        damp_[s] += 0.30f * (rd - damp_[s]);
        const float hp = damp_[s] - dcX_[s] + 0.995f * dcY_[s];
        dcX_[s] = damp_[s];
        dcY_[s] = hp;
        float v = hp * kFb + x;
        if (v > 1.0f) v = 1.0f;
        if (v < -1.0f) v = -1.0f;
        buf_[s][w_[s]] = (int16_t)(v * 32767.0f);
        if (++w_[s] >= kMaxLen) w_[s] = 0;
        sum += rd;
      }
      out[i] += sum * kHalo;
    }
  }

 private:
  static constexpr float kRatio[kNumStrings] = {1.0f, 1.5f, 2.0f, 2.25f, 3.0f};
  static constexpr int kMaxLen = 896;  // sr/55 ≈ 873 for the 1/1 string
  static constexpr float kMinLen = 24.0f;
  static constexpr float kFb = 0.995f;
  static constexpr float kExcite = 0.13f;
  static constexpr float kHalo = 0.35f;

  float lenFor(float root, int s) const {
    float l = sr_ / (root * kRatio[s]);
    if (l < kMinLen) l = kMinLen;
    if (l > (float)kMaxLen - 4.0f) l = (float)kMaxLen - 4.0f;
    return l;
  }

  int16_t buf_[kNumStrings][kMaxLen];
  float len_[kNumStrings] = {0};
  float damp_[kNumStrings] = {0};
  float dcX_[kNumStrings] = {0};
  float dcY_[kNumStrings] = {0};
  int w_[kNumStrings] = {0};
  float sr_ = 48000.0f;
  std::atomic<float> root_{220.0f};
  std::atomic<bool> enabled_{true};
};

}  // namespace ga
