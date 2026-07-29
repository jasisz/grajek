// Mono reverb (Freeverb topology: 8 damped combs + 4 allpasses), portable.
// Fixed-size static buffers (~55 KB of float state) — fits ESP32-S3 internal
// RAM, no allocation. Comb/allpass lengths are the classic Freeverb tunings
// scaled from 44.1 kHz to 48 kHz.
//
// Threading: setters are atomic (control thread); process() audio thread.
// process() reads the buffer and ADDS the wet signal in place (dry stays).
#pragma once
#include <stdint.h>
#include <atomic>

namespace ga {

class Reverb {
 public:
  void init(float sampleRate);

  // --- control thread ---
  void setWet(float w);    // 0..1; 0 disables processing entirely
  float wet() const { return wet_.load(std::memory_order_relaxed); }
  void setRoom(float r);   // 0..1 -> comb feedback (tail length)
  void setDamp(float d);   // 0..1 -> high-frequency absorption

  // --- audio thread: buf <- buf + wet reverb of buf ---
  void process(float* buf, int n);

 private:
  static constexpr int kNumCombs = 8;
  static constexpr int kNumAllpass = 4;
  static constexpr int kCombLen[kNumCombs] = {1215, 1293, 1390, 1476,
                                              1548, 1623, 1695, 1760};
  static constexpr int kAllpassLen[kNumAllpass] = {245, 371, 480, 605};
  static constexpr int kCombTotal =
      1215 + 1293 + 1390 + 1476 + 1548 + 1623 + 1695 + 1760;
  static constexpr int kAllpassTotal = 245 + 371 + 480 + 605;

  float comb_[kCombTotal] = {0};
  float allpass_[kAllpassTotal] = {0};
  float combFilt_[kNumCombs] = {0};  // damping one-pole state per comb
  int combIdx_[kNumCombs] = {0};
  int allpassIdx_[kNumAllpass] = {0};
  int combOff_[kNumCombs] = {0};      // start offsets into comb_
  int allpassOff_[kNumAllpass] = {0};

  float kHpWet_ = 0.026f;  // one-pole HP ~200 Hz on the wet sum (anti-mud)
  float hpLp_ = 0.0f;
  std::atomic<float> wet_{0.35f};
  std::atomic<float> room_{0.84f};  // stored as comb feedback
  std::atomic<float> damp_{0.18f};  // brighter tail — air, not fog
};

}  // namespace ga
