// EchoTape — a Frippertronics-style ambient tape loop, header-only.
//
// The generosity layer of the instrument: it runs ALWAYS, needs no buttons,
// and everything played comes back a few seconds later, quieter, fading over
// a handful of repeats. Single fixed-length tape: read the past, mix it into
// the output, write back (decayed past + new input) under the same head.
//
// Same threading contract as the rest of grajek_audio: setters are atomic
// and safe from the control thread; process() runs on the audio thread.
// Buffer is caller-provided int16, like ga::Looper.
#pragma once
#include <stdint.h>
#include <string.h>
#include <atomic>

namespace ga {

class EchoTape {
 public:
  void init(int16_t* buf, uint32_t frames) {
    buf_ = buf;
    len_ = frames;
    pos_ = 0;
    memset(buf_, 0, frames * sizeof(int16_t));
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
  void clearTape() { clearReq_.store(true, std::memory_order_release); }

  // --- audio thread: reads `in`, ADDS the tape's past into `out`,
  // records the present onto the tape. in/out may alias. ---
  void process(const float* in, float* out, int n) {
    if (clearReq_.exchange(false, std::memory_order_acquire)) {
      memset(buf_, 0, len_ * sizeof(int16_t));
      pos_ = 0;
    }
    if (!enabled_.load(std::memory_order_relaxed) || len_ == 0) return;
    const float fb = feedback_.load(std::memory_order_relaxed);
    const float lvl = level_.load(std::memory_order_relaxed);
    for (int i = 0; i < n; ++i) {
      const float dry = in[i];  // read before touching out — they may alias
      const float past = (float)buf_[pos_] * (1.0f / 32768.0f);
      out[i] = dry + past * lvl;
      float v = (past * fb + dry) * 32767.0f;
      if (v > 32767.0f) v = 32767.0f;
      if (v < -32768.0f) v = -32768.0f;
      buf_[pos_] = (int16_t)v;
      if (++pos_ >= len_) pos_ = 0;
    }
  }

 private:
  int16_t* buf_ = nullptr;
  uint32_t len_ = 0;
  uint32_t pos_ = 0;  // audio-thread only
  std::atomic<bool> enabled_{true};
  std::atomic<bool> clearReq_{false};
  std::atomic<float> feedback_{0.55f};
  std::atomic<float> level_{0.5f};
};

}  // namespace ga
