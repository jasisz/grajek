// Lightweight mono chorus: two independently wandering fractional-delay taps.
//
// The delay line is fixed at 1024 floats (exactly 4 KB at the supported
// targets), lives inside the object and never allocates. At Grajek's 48 kHz
// rate the taps sit around 11 and 16 ms and wander by up to +/-3 ms. Their
// different LFO rates keep the result from collapsing into one obvious pitch
// wobble.
//
// Threading follows the rest of grajek_audio: setDepth() is safe on the
// control thread; process() belongs to the audio thread. At settled depth zero
// output is a true, sample-exact bypass that returns immediately, and the ring
// is cleared when depth comes back so re-enabling cannot resurrect an old
// audio fragment.
#pragma once

#include <atomic>
#include <math.h>
#include <string.h>

namespace ga {

class Chorus {
 public:
  static constexpr int kBufferFrames = 1024;
  static constexpr int kBufferBytes = kBufferFrames * (int)sizeof(float);
  static_assert(kBufferBytes <= 4096, "chorus delay buffer exceeds 4 KB");

  void init(float sampleRate) {
    sr_ = sampleRate > 1000.0f ? sampleRate : 48000.0f;
    memset(delay_, 0, sizeof(delay_));
    write_ = 0;
    phaseA_ = 0.0f;
    phaseB_ = 0.5f;

    const float maxDelay = (float)kBufferFrames - 3.0f;
    // Preserve two distinct taps even if a host uses a rate above 48 kHz:
    // scale the whole geometry down together instead of pinning both heads to
    // the end of the finite ring.
    const float requestedMax = 0.019f * sr_;  // 16 ms base + 3 ms sweep
    const float geometryScale =
        requestedMax > maxDelay ? maxDelay / requestedMax : 1.0f;
    baseA_ = clampDelay(0.011f * sr_ * geometryScale, maxDelay);
    baseB_ = clampDelay(0.016f * sr_ * geometryScale, maxDelay);
    if (baseB_ <= baseA_ + 2.0f)
      baseB_ = clampDelay(baseA_ + 2.0f, maxDelay);

    // Keep both modulated heads at least two frames away from the write head;
    // fractional reads therefore never touch the sample currently written.
    sweep_ = 0.003f * sr_ * geometryScale;
    const float roomBefore = baseA_ - 2.0f;
    const float roomAfter = maxDelay - baseB_;
    if (sweep_ > roomBefore) sweep_ = roomBefore;
    if (sweep_ > roomAfter) sweep_ = roomAfter;
    if (sweep_ < 0.0f) sweep_ = 0.0f;

    stepA_ = 0.23f / sr_;
    stepB_ = 0.31f / sr_;
    depthCoef_ = 1.0f - expf(-1.0f / (0.010f * sr_));
    currentDepth_ = 0.0f;
    bypassed_ = true;
    depth_.store(0.0f, std::memory_order_relaxed);
  }

  // --- control thread ---
  void setDepth(float depth) {
    // The inverted comparison also turns NaN into a safe bypass value.
    if (!(depth > 0.0f)) depth = 0.0f;
    if (depth > 1.0f) depth = 1.0f;
    depth_.store(depth, std::memory_order_relaxed);
  }
  float depth() const { return depth_.load(std::memory_order_relaxed); }

  // --- audio thread: dry signal plus a quiet average of two delayed taps ---
  void process(float* buf, int n) {
    if (!buf || n <= 0) return;
    const float targetDepth = depth_.load(std::memory_order_relaxed);
    // Settled bypass leaves the loop entirely rather than shovelling the dry
    // stream through the ring for nothing — five of the seven timbres never
    // ask for chorus at all, and they were paying for it every sample. The
    // "cannot resurrect an old fragment" guarantee is kept by clearing the
    // ring on the way back in instead of by continuously overwriting it.
    if (targetDepth <= 0.0f && currentDepth_ == 0.0f) {
      bypassed_ = true;
      return;
    }
    if (bypassed_) {
      memset(delay_, 0, sizeof(delay_));
      write_ = 0;
      bypassed_ = false;
    }
    for (int i = 0; i < n; ++i) {
      const float dry = buf[i];
      currentDepth_ += depthCoef_ * (targetDepth - currentDepth_);
      if (targetDepth <= 0.0f && currentDepth_ < 1e-5f)
        currentDepth_ = 0.0f;

      float wetSignal = 0.0f;
      if (currentDepth_ > 0.0f) {
        const float excursion = sweep_ * currentDepth_;
        const float tapA = readDelay(baseA_ + excursion * lfo(phaseA_));
        const float tapB = readDelay(baseB_ + excursion * lfo(phaseB_));
        wetSignal = 0.38f * currentDepth_ * 0.5f * (tapA + tapB);
      }
      delay_[write_] = dry;
      write_ = (write_ + 1) & (kBufferFrames - 1);

      // No feedback: the chorus adds width/motion without growing a tail of
      // its own. The global soft clip later in the chain owns final headroom.
      buf[i] = dry + wetSignal;

      phaseA_ += stepA_;
      phaseB_ += stepB_;
      if (phaseA_ >= 1.0f) phaseA_ -= 1.0f;
      if (phaseB_ >= 1.0f) phaseB_ -= 1.0f;
    }
  }

 private:
  static float clampDelay(float frames, float maximum) {
    if (frames < 2.0f) return 2.0f;
    if (frames > maximum) return maximum;
    return frames;
  }

  // Smooth, inexpensive sine approximation for a sub-audio-rate modulator.
  // It is periodic with matching slopes at the wrap, unlike a triangle LFO.
  static float lfo(float phase) {
    const float x = phase * 2.0f - 1.0f;
    float y = 4.0f * x * (1.0f - fabsf(x));
    y += 0.225f * (y * fabsf(y) - y);
    return y;
  }

  float readDelay(float frames) const {
    float read = (float)write_ - frames;
    if (read < 0.0f) read += (float)kBufferFrames;
    const int i0 = (int)read;
    const int i1 = (i0 + 1) & (kBufferFrames - 1);
    const float frac = read - (float)i0;
    return delay_[i0] + (delay_[i1] - delay_[i0]) * frac;
  }

  static_assert((kBufferFrames & (kBufferFrames - 1)) == 0,
                "chorus ring size must be a power of two");
  float delay_[kBufferFrames] = {0};
  int write_ = 0;
  float sr_ = 48000.0f;
  float baseA_ = 528.0f;
  float baseB_ = 768.0f;
  float sweep_ = 144.0f;
  float phaseA_ = 0.0f;
  float phaseB_ = 0.5f;
  float stepA_ = 0.23f / 48000.0f;
  float stepB_ = 0.31f / 48000.0f;
  float depthCoef_ = 1.0f / 1200.0f;
  float currentDepth_ = 0.0f;
  bool bypassed_ = true;
  std::atomic<float> depth_{0.0f};
};

}  // namespace ga
