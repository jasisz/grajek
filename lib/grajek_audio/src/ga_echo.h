// EchoTape — a Frippertronics-style ambient tape loop, header-only.
//
// The generosity layer of the instrument: it runs ALWAYS, needs no buttons,
// and everything played comes back a few seconds later, quieter, fading over
// a handful of repeats.
//
// v2 — a tape that FORGETS instead of copying: every pass through the
// feedback path darkens (one-pole lowpass ~4.5 kHz), sheds mud (one-pole
// highpass ~150 Hz, doubling as a DC blocker), and saturates softly; the
// read head wanders slowly (tape wow), and a second read tap at the golden
// ratio of the loop length (with its own slow drift) breaks the metronomic
// return — repeats age into phrases instead of echoing. When the instrument
// discovers a steady human pulse, that second head crossfades to a dotted
// eighth of the learned period; when the pulse lets go, it slowly returns to
// the wandering golden position.
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
    // aging-filter coefficients (one-pole); LP at 4.5 kHz — repeats age
    // noticeably but the tape stops sounding like a cellar
    kLp_ = 1.0f - expf(-kTau * 4500.0f / sampleRate);
    kHp_ = 1.0f - expf(-kTau * 150.0f / sampleRate);
    lp_ = hpLp_ = 0.0f;
    wob_ = wobTarget_ = 0.0f;
    driftPhase_ = 0.0f;
    rhythmicOffset_ = 0.618034f * (float)frames;
    rhythmicMix_ = 0.0f;
    rng_ = 0x9E3779B9u;
    enabled_.store(true);
    clearReq_.store(false);
    rhythmicDelaySec_.store(0.0f, std::memory_order_relaxed);
    rhythmicRequested_.store(false, std::memory_order_relaxed);
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
  // Let the second read head answer the pulse without turning the instrument
  // into a BPM-controlled delay. A dotted eighth (3/4 period) is long enough
  // to be heard as a reply; at very fast tempi it rests continuously at
  // 280 ms instead of collapsing into a metallic comb. A continuous floor
  // avoids octave jumps when the learned period hovers near the boundary.
  // Calling with pulseTicking=false starts a slow crossfade back to the free
  // golden-ratio head. Safe to call from the control thread.
  void setRhythmicTap(float periodSec, bool pulseTicking) {
    float delaySec = 0.75f * periodSec;
    const bool valid = isfinite(delaySec) && delaySec > 0.0f;
    if (valid) {
      if (delaySec < 0.280f) delaySec = 0.280f;
      rhythmicDelaySec_.store(delaySec, std::memory_order_relaxed);
    }
    rhythmicRequested_.store(pulseTicking && valid,
                              std::memory_order_relaxed);
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
    bool rhythmicRequested =
        rhythmicRequested_.load(std::memory_order_relaxed);
    float rhythmicTarget =
        rhythmicDelaySec_.load(std::memory_order_relaxed) * sr_;
    if (!isfinite(rhythmicTarget) || rhythmicTarget < 1.0f) {
      rhythmicRequested = false;
      rhythmicTarget = 1.0f;
    }
    const float maxOffset = len_ > 1 ? (float)(len_ - 1) : 1.0f;
    if (rhythmicTarget > maxOffset) rhythmicTarget = maxOffset;

    // The two positions are always read independently and crossfaded. Moving
    // one head all the way from golden to rhythmic would scrub through the
    // tape (a conspicuous pitch dive); a crossfade changes the metre quietly.
    // Period corrections within the rhythmic head follow softly, tape-style.
    if (rhythmicRequested && rhythmicMix_ < 0.0001f)
      rhythmicOffset_ = rhythmicTarget;  // move the inaudible head for free
    // While the head is neither wanted nor still fading out, none of its
    // state can affect a sample: line 118 re-seats the offset the moment it
    // engages. Skipping the whole section also keeps an expf and a divide out
    // of the block prologue on every preset that never locks to a pulse.
    const bool pllIdle = !rhythmicRequested && rhythmicMix_ <= 0.0f;
    const float offsetFollow =
        pllIdle ? 0.0f
                : 1.0f - expf(-1.0f / (0.35f * sr_));  // live PLL corrections
    const float mixStep =
        pllIdle ? 0.0f : 1.0f / ((rhythmicRequested ? 0.75f : 1.50f) * sr_);

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
    const float off2 =
        0.618034f * (float)len_ + driftAmp * sinf(driftPhase_);

    for (int i = 0; i < n; ++i) {
      const float dry = in[i];  // read before touching out — they may alias
      // reading AT the write position (pre-write) = exactly one tape length
      // of delay; the wow term shortens it by a few wandering samples
      const float past = readAt((float)pos_ - wob_);
      if (!pllIdle) {
        rhythmicOffset_ += offsetFollow * (rhythmicTarget - rhythmicOffset_);
        if (rhythmicRequested) {
          rhythmicMix_ += mixStep;
          if (rhythmicMix_ > 1.0f) rhythmicMix_ = 1.0f;
        } else {
          rhythmicMix_ -= mixStep;
          if (rhythmicMix_ < 0.0f) rhythmicMix_ = 0.0f;
        }
      }
      // The rhythmic head only exists while the player's pulse is confident.
      // Interpolating it when it is fully crossed out was a third of this
      // tape's cost, paid on every preset, for a sample worth nothing.
      const float goldenTap = readAt((float)pos_ - off2);
      const float tap2 =
          rhythmicMix_ > 0.0f
              ? goldenTap + rhythmicMix_ *
                                (readAt((float)pos_ - rhythmicOffset_) -
                                 goldenTap)
              : goldenTap;

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
  // Single precision on purpose: the ESP32-S3 has no double FPU, so every
  // double operation here was a software emulation call, three times per
  // sample. At a 48000-frame tape a float still resolves position to ~0.006
  // of a sample, far finer than the wow that modulates it.
  //
  // Callers keep every offset inside (-len_, len_), so one conditional wrap
  // is enough. The old `while` loops were also a hang waiting to happen: a
  // non-finite index skipped both tests, and a large one would have spun.
  float readAt(float idx) const {
    if (!(idx > -(float)len_ && idx < 2.0f * (float)len_)) return 0.0f;
    if (idx < 0.0f) idx += (float)len_;
    if (idx >= (float)len_) idx -= (float)len_;
    const uint32_t i0 = (uint32_t)idx;
    if (i0 >= len_) return 0.0f;
    uint32_t i1 = i0 + 1;
    if (i1 >= len_) i1 = 0;
    const float frac = idx - (float)i0;
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
  float rhythmicOffset_ = 0.0f;         // rhythmic head, audio-thread only
  float rhythmicMix_ = 0.0f;            // golden -> rhythmic crossfade
  uint32_t rng_ = 1;
  std::atomic<bool> enabled_{true};
  std::atomic<bool> clearReq_{false};
  std::atomic<float> feedback_{0.55f};
  std::atomic<float> level_{0.5f};
  std::atomic<float> wowDepth_{32.0f};
  std::atomic<float> rhythmicDelaySec_{0.0f};
  std::atomic<bool> rhythmicRequested_{false};
};

}  // namespace ga
