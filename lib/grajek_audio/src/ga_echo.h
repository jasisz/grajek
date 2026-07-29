// EchoTape — a Frippertronics-style ambient tape loop, header-only.
//
// The generosity layer of the instrument: it runs ALWAYS, needs no buttons,
// and everything played comes back a few seconds later, quieter, fading over
// a handful of repeats.
//
// v2 — a tape that FORGETS instead of copying: every pass through the
// feedback path darkens (one-pole lowpass ~3 kHz), sheds mud (one-pole
// highpass ~150 Hz, doubling as a DC blocker), and saturates softly; the
// read head wanders slowly (tape wow), and a second read tap at the golden
// ratio of the loop length (with its own slow drift) breaks the metronomic
// return — repeats age into phrases instead of echoing.
//
// Same threading contract as the rest of grajek_audio: setters are atomic
// and safe from the control thread; process() runs on the audio thread.
// Buffer is caller-provided int16, like ga::Looper.
#pragma once
#include <stdint.h>
#include <math.h>
#include <string.h>
#include <atomic>

#include "ga_dsp.h"

namespace ga {

class EchoTape {
 public:
  void init(int16_t* buf, uint32_t frames, float sampleRate) {
    buf_ = buf;
    len_ = frames;
    pos_ = 0;
    sr_ = sampleRate;
    memset(buf_, 0, frames * sizeof(int16_t));
    // aging-filter coefficients (one-pole)
    kLp_ = 1.0f - expf(-kTau * 3000.0f / sampleRate);
    kHp_ = 1.0f - expf(-kTau * 150.0f / sampleRate);
    lp_ = hpLp_ = 0.0f;
    wob_ = wobTarget_ = 0.0f;
    driftPhase_ = 0.0f;
    rng_ = 0x9E3779B9u;
    enabled_.store(true);
    clearReq_.store(false);
  }

  // --- control thread ---
  void setEnabled(bool on) { enabled_.store(on, std::memory_order_relaxed); }
  bool enabled() const { return enabled_.load(std::memory_order_relaxed); }
  void setFeedback(float f) {  // how much survives each pass (0..0.95)
    if (f < 0.0f) f = 0.0f;
    if (f > 0.95f) f = 0.95f;
    feedback_.store(f, std::memory_order_relaxed);
  }
  void setLevel(float l) {  // how loud the past is vs the present
    if (l < 0.0f) l = 0.0f;
    if (l > 1.0f) l = 1.0f;
    level_.store(l, std::memory_order_relaxed);
  }
  void setWowDepth(float samples) {  // 0 disables the wandering head
    if (samples < 0.0f) samples = 0.0f;
    if (samples > 96.0f) samples = 96.0f;
    wowDepth_.store(samples, std::memory_order_relaxed);
  }
  void clearTape() { clearReq_.store(true, std::memory_order_release); }

  // --- audio thread: reads `in`, ADDS the tape's past into `out`,
  // records the present onto the tape. in/out may alias. ---
  void process(const float* in, float* out, int n) {
    if (clearReq_.exchange(false, std::memory_order_acquire)) {
      memset(buf_, 0, len_ * sizeof(int16_t));
      pos_ = 0;
      lp_ = hpLp_ = 0.0f;
    }
    if (!enabled_.load(std::memory_order_relaxed) || len_ == 0) return;
    const float fb = feedback_.load(std::memory_order_relaxed);
    const float lvl = level_.load(std::memory_order_relaxed);
    const float wowDepth = wowDepth_.load(std::memory_order_relaxed);

    // block-rate modulation: the wow target performs a slow random walk,
    // the golden tap drifts on its own ~0.07 Hz sine (±30 ms)
    if (wowDepth > 0.0f) {
      wobTarget_ += (rnd01() - 0.5f) * 0.2f * wowDepth;
      if (wobTarget_ < 0.0f) wobTarget_ = 0.0f;
      if (wobTarget_ > wowDepth) wobTarget_ = wowDepth;
      wob_ += 0.1f * (wobTarget_ - wob_);
    } else {
      wob_ = wobTarget_ = 0.0f;
    }
    driftPhase_ += kTau * 0.07f * (float)n / sr_;
    if (driftPhase_ > kTau) driftPhase_ -= kTau;
    float driftAmp = 0.030f * sr_;  // ±30 ms
    if (driftAmp > 0.25f * (float)len_) driftAmp = 0.25f * (float)len_;
    const double off2 =
        0.618034 * (double)len_ + (double)(driftAmp * sinf(driftPhase_));

    for (int i = 0; i < n; ++i) {
      const float dry = in[i];  // read before touching out — they may alias
      // reading AT the write position (pre-write) = exactly one tape length
      // of delay; the wow term shortens it by a few wandering samples
      const float past = readAt((double)pos_ - (double)wob_);
      const float tap2 = readAt((double)pos_ - off2);

      // aging: each pass darkens, sheds mud, saturates a little
      lp_ += kLp_ * (past - lp_);
      hpLp_ += kHp_ * (lp_ - hpLp_);
      const float aged = softClip((lp_ - hpLp_) * fb);

      out[i] = dry + (past + 0.3f * tap2) * lvl;

      float w = (dry + aged + 0.2f * tap2) * 32767.0f;  // loop gain < 1
      if (w > 32767.0f) w = 32767.0f;
      if (w < -32768.0f) w = -32768.0f;
      buf_[pos_] = (int16_t)w;
      if (++pos_ >= len_) pos_ = 0;
    }
  }

 private:
  float readAt(double idx) const {
    while (idx < 0.0) idx += (double)len_;
    while (idx >= (double)len_) idx -= (double)len_;
    const uint32_t i0 = (uint32_t)idx;
    uint32_t i1 = i0 + 1;
    if (i1 >= len_) i1 = 0;
    const float frac = (float)(idx - (double)i0);
    return ((float)buf_[i0] * (1.0f - frac) + (float)buf_[i1] * frac) *
           (1.0f / 32768.0f);
  }
  float rnd01() {
    rng_ ^= rng_ << 13;
    rng_ ^= rng_ >> 17;
    rng_ ^= rng_ << 5;
    return (float)(rng_ >> 8) * (1.0f / 16777216.0f);
  }

  int16_t* buf_ = nullptr;
  uint32_t len_ = 0;
  uint32_t pos_ = 0;  // write head, audio-thread only
  float sr_ = 48000.0f;
  float kLp_ = 0.3f, kHp_ = 0.02f;
  float lp_ = 0.0f, hpLp_ = 0.0f;       // aging-filter states
  float wob_ = 0.0f, wobTarget_ = 0.0f; // read-head wow (samples)
  float driftPhase_ = 0.0f;             // golden-tap drift LFO
  uint32_t rng_ = 1;
  std::atomic<bool> enabled_{true};
  std::atomic<bool> clearReq_{false};
  std::atomic<float> feedback_{0.55f};
  std::atomic<float> level_{0.5f};
  std::atomic<float> wowDepth_{32.0f};
};

}  // namespace ga
