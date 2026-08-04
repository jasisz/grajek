// Voice: a bank of sine partials + ADSR + portamento.
// Aesthetic: pure tones with slight random partial detune (slow beating).
#pragma once
#include <stdint.h>
#include "ga_dsp.h"
#include "ga_adsr.h"
#include "ga_svf.h"

namespace ga {

// The original instrument remains an additive partial bank.  The two broad-
// spectrum modes are extra colours, not a replacement engine: they reuse the
// same voice phases, envelope, pitch tracking and child-safe output chain.
enum class OscillatorKind : uint8_t { Additive = 0, FatSaw, Pulse };

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

  OscillatorKind oscillator = OscillatorKind::Additive;
  float oscGain             = 1.0f;
  float pulseWidth          = 0.5f;
  float pwmDepth            = 0.0f;
  float pwmRateHz           = 0.35f;

  // Paraphonic colour stage.  The existing global SVF is opened by a real
  // keyboard attack, so broad saw/pulse sources bloom without adding ten
  // per-voice filters.  Existing additive presets keep neutral values.
  FilterMode filterMode     = FilterMode::Lowpass;
  float filterScale         = 1.0f;
  float filterRes           = 0.15f;
  float filterEnvOct        = 0.0f;
  float filterEnvAttack     = 0.005f;
  float filterEnvDecay      = 0.35f;
  // How far the cutoff follows the played pitch, in octaves per octave.
  // The additive presets darken their upper partials as pitch rises (see the
  // brightness tracking in Engine::handle), but gain[] is meaningless to the
  // saw/pulse paths.  Key tracking is how those broad sources get the same
  // manners: below 1.0 the cutoff climbs slower than the note, so the top of
  // the keyboard stays sweet, while a fixed cutoff would either swallow the
  // high notes whole or let them scream.
  float filterKeyTrack      = 0.0f;

  // Mild bus saturation supplies density before the same hard safety ceiling.
  float drive               = 1.0f;
  // Short, envelope-gated air/strike transient.  Zero leaves the RNG out of
  // the sample loop entirely.
  float attackNoise         = 0.0f;
  float noiseDecay          = 0.02f;
  // Device/host effect adapter reads this when the preset changes.
  float chorusDepth         = 0.0f;
  // Loudness match between presets. The timbres are NOT equally loud to the
  // ear even at equal RMS, and the sympathetic strings widen the gap further:
  // a near-sine excites at most one resonator, a saw excites all five and
  // feeds the reverb across the band, so the bright presets collect a large
  // halo for free. This trim pays that difference back, measured A-weighted
  // through the whole chain rather than guessed from peak levels.
  float outputTrim          = 1.0f;
};

// 0 = PURE (clean tone), 1 = ORGAN (octaves, strong beating, slow envelope),
// 2 = REED (odd harmonics, fast attack), 3 = CHIME (music-box ping melting
// into a pure tone), 4 = MUSICBOX (percussive plucked tine), 5 = WARM
// (filter-bloomed fat saw), 6 = HOLLOW (slowly breathing pulse).
// Persisted settings use these indices: append only, never reorder.
constexpr int kNumTimbrePresets = 7;
Timbre timbrePreset(int idx);

class Voice {
 public:
  void init(float sampleRate, uint32_t seed);
  void noteOn(int32_t id, float cents, float vel, const Timbre& t,
              float glideSec, uint32_t age, bool persistent = false,
              bool hasGlideFrom = false, float glideFromCents = 0.0f);
  void noteOff() { env_.noteOff(); persistent_ = false; }
  void kill() { env_.reset(); persistent_ = false; }
  bool active() const { return env_.active(); }
  bool releasing() const { return env_.releasing(); }
  int32_t id() const { return id_; }
  uint32_t age() const { return age_; }
  bool persistent() const { return persistent_; }
  float currentCents() const { return cents_; }
  // Includes the preset trim: the engine steals the QUIETEST releasing voice,
  // and without it a trimmed-down timbre looked louder than it sounds.
  float level() const { return env_.level() * vel_ * trim_; }

  // Renders and ADDS into the mono buffer. bendRatio = global offset (IMU).
  // bassVoicing: psychoacoustic bass for tiny speakers — below ~250 Hz the
  // fundamental's energy moves into partials 2-3 and the ear rebuilds the
  // missing fundamental (virtual pitch); a 3 cm driver never gets asked
  // for air it cannot move.
  // bassVoicing is a 0..1 blend so the effect can be faded rather than
  // switched; 0 disables it entirely and costs nothing extra.
  void render(float* out, int n, float baseHz, float bendRatio,
              float bassVoicing);

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
  // Cached transcendentals: recomputed only when their inputs move.
  float detRatio_[Timbre::kPartials] = {1.0f, 1.0f, 1.0f, 1.0f};
  float pdecayMul_[Timbre::kPartials] = {1.0f, 1.0f, 1.0f, 1.0f};
  float noiseMul_ = 1.0f;
  float centsRatio_ = 1.0f;
  float ratioCents_ = 1e30f;
  int decayN_ = 0;
  float shimmer_ = 1.0f;
  float detLimit_ = 0.3f;
  OscillatorKind oscillator_ = OscillatorKind::Additive;
  float oscGain_ = 1.0f;
  float pulseWidth_ = 0.5f;
  float pwmDepth_ = 0.0f;
  float pwmRateHz_ = 0.35f;
  float pwmPhase_ = 0.0f;  // turns
  float pulseWidthCurrent_ = 0.5f;
  float noiseAmp_ = 0.0f;
  float noiseDecay_ = 0.02f;
  float trim_ = 1.0f;
  bool bassOk_ = true;  // partials 2-3 imply f0 (see noteOn) — else the
                        // virtual-pitch shift would rebuild the octave
  float cents_ = 0.0f;
  float targetCents_ = 0.0f;
  float glideSec_ = 0.0f;
  float vel_ = 1.0f;
  int32_t id_ = -1;
  uint32_t age_ = 0;
  bool persistent_ = false;
  uint32_t rng_ = 1;
};

}  // namespace ga
