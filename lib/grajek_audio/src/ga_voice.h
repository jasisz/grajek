// Voice: a bank of sine partials + ADSR + portamento.
// Aesthetic: pure tones with slight random partial detune (slow beating).
#pragma once
#include <stdint.h>
#include "ga_dsp.h"
#include "ga_adsr.h"

namespace ga {

struct Timbre {
  static constexpr int kPartials = 4;
  float ratio[kPartials]  = {1.0f, 2.0f, 3.0f, 4.0f};    // f0 multipliers
  float gain[kPartials]   = {0.9f, 0.12f, 0.05f, 0.02f}; // partial amplitudes
  // Per-partial decay time constant in seconds (<= 0 = the partial holds).
  // A bright attack whose upper partials melt away = bell/kalimba character.
  float pdecay[kPartials] = {0.0f, 0.0f, 0.0f, 0.0f};
  float detuneCents       = 0.3f;   // max random detune of partials > 1
  // Breathing: scales the per-partial slow amplitude LFOs and detune drift —
  // a held tone stays a process instead of a frozen sum of sines.
  float shimmer           = 1.0f;
  float attack  = 0.05f;
  float decay   = 0.3f;
  float sustain = 0.85f;
  float release = 0.6f;
};

// 0 = PURE (clean tone), 1 = DRONE (octaves, strong beating, slow envelope),
// 2 = REED (odd harmonics, fast attack), 3 = CHIME (music-box ping melting
// into a pure tone), 4 = MUSICBOX (percussive plucked tine — bright, ends
// by itself, made for rhythmic playing)
constexpr int kNumTimbrePresets = 5;
Timbre timbrePreset(int idx);

class Voice {
 public:
  void init(float sampleRate, uint32_t seed);
  void noteOn(int32_t id, float cents, float vel, const Timbre& t,
              float glideSec, uint32_t age);
  void noteOff() { env_.noteOff(); }
  void kill() { env_.reset(); }
  bool active() const { return env_.active(); }
  bool releasing() const { return env_.releasing(); }
  int32_t id() const { return id_; }
  uint32_t age() const { return age_; }
  float currentCents() const { return cents_; }

  // Renders and ADDS into the mono buffer. bendRatio = global offset (IMU).
  void render(float* out, int n, float baseHz, float bendRatio);

 private:
  uint32_t rng();
  float rnd11();  // random value in -1..1

  ADSR env_;
  float sr_ = 48000.0f;
  uint32_t phase_[Timbre::kPartials] = {0};
  float ratio_[Timbre::kPartials] = {1.0f};
  float gain_[Timbre::kPartials] = {0.0f};
  float det_[Timbre::kPartials] = {0.0f};    // detune in cents (wanders)
  float pdecay_[Timbre::kPartials] = {0.0f}; // per-partial decay tau (s)
  float pamp_[Timbre::kPartials] = {1.0f};   // per-partial decaying amplitude
  float lfoPhase_[Timbre::kPartials] = {0.0f}; // breathing LFOs (turns)
  float lfoInc_[Timbre::kPartials] = {0.0f};   // turns per sample
  float shimmer_ = 1.0f;
  float detLimit_ = 0.3f;
  float cents_ = 0.0f;
  float targetCents_ = 0.0f;
  float glideSec_ = 0.0f;
  float vel_ = 1.0f;
  int32_t id_ = -1;
  uint32_t age_ = 0;
  uint32_t rng_ = 1;
};

}  // namespace ga
