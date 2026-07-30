// Voice: a bank of sine partials + ADSR + portamento.
// Aesthetic: pure tones with slight random partial detune (slow beating).
#pragma once
#include <stdint.h>
#include "ga_dsp.h"
#include "ga_adsr.h"

namespace ga {

// A caught grain of the player's voice (or any sound): caller-owned buffer,
// looped with a crossfade and tape-repitched onto the grid. Published to the
// engine via an atomic pointer; the struct and data must stay valid while
// any voice may still be reading them (double-buffer on the caller side).
struct VoiceSample {
  const int16_t* data;
  uint32_t frames;
  float rootHz;        // detected fundamental of the grain
  float periodFrames;  // source pitch period in samples; > 0 enables the
                       // formant-preserving PSOLA mode (no chipmunk)
};

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
  // VOICE preset: play the caught sample instead of sine partials
  bool useSample = false;
};

// 0 = PURE (clean tone), 1 = DRONE (octaves, strong beating, slow envelope),
// 2 = REED (odd harmonics, fast attack), 3 = CHIME (music-box ping melting
// into a pure tone), 4 = MUSICBOX (percussive plucked tine — bright, ends
// by itself, made for rhythmic playing), 5 = VOICE (the caught grain of the
// player's own voice, repitched onto the grid)
constexpr int kNumTimbrePresets = 6;
Timbre timbrePreset(int idx);

class Voice {
 public:
  void init(float sampleRate, uint32_t seed);
  void noteOn(int32_t id, float cents, float vel, const Timbre& t,
              float glideSec, uint32_t age, const VoiceSample* smp = nullptr);
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
  const VoiceSample* smp_ = nullptr;  // non-null = play the sample instead
  float smpPos_ = 0.0f;
  // PSOLA state: up to 3 overlapping pitch-synchronous grains
  float gsStart_[3] = {0.0f};  // source anchor of each flying grain
  float gsPos_[3] = {0.0f};    // position inside the grain window
  bool gsOn_[3] = {false};
  int gsNext_ = 0;
  float outCount_ = 0.0f;  // samples until the next grain launch
  float srcScan_ = 0.0f;   // slow scan through the source material
  float cents_ = 0.0f;
  float targetCents_ = 0.0f;
  float glideSec_ = 0.0f;
  float vel_ = 1.0f;
  int32_t id_ = -1;
  uint32_t age_ = 0;
  uint32_t rng_ = 1;
};

}  // namespace ga
