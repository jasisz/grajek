#include "ga_voice.h"

namespace ga {
namespace {

constexpr float kPhaseToTurns = 1.0f / 4294967296.0f;
constexpr int kFatSawCount = 2;

// PolyBLEP removes the discontinuity of saw/pulse edges without keeping a
// second bank of float wavetables in scarce internal RAM.  Away from an edge
// it is exactly zero, so the common path stays small.
// dt and its reciprocal are constant for a whole block, so both are hoisted
// out of the sample loop by the caller: the audio task on the ESP32 cannot
// afford a uint-to-float conversion and a divide per oscillator per sample.
inline float polyBlep(float t, float dt, float invDt) {
  if (!(dt > 0.0f) || dt >= 0.5f) return 0.0f;
  if (t < dt) {
    t *= invDt;
    return t + t - t * t - 1.0f;
  }
  if (t > 1.0f - dt) {
    t = (t - 1.0f) * invDt;
    return t * t + t + t + 1.0f;
  }
  return 0.0f;
}

inline float bandSaw(uint32_t phase, float dt, float invDt) {
  const float t = (float)phase * kPhaseToTurns;
  return (2.0f * t - 1.0f) - polyBlep(t, dt, invDt);
}

inline float bandPulse(uint32_t phase, float dt, float invDt, float width) {
  const float t = (float)phase * kPhaseToTurns;
  float y = t < width ? 1.0f : -1.0f;
  y += polyBlep(t, dt, invDt);
  float fall = t - width;
  if (fall < 0.0f) fall += 1.0f;
  y -= polyBlep(fall, dt, invDt);
  // A variable-width bipolar pulse otherwise has mean 2*w-1. Removing it
  // here keeps slow PWM from pumping DC through strings, tape and reverb.
  return y - (2.0f * width - 1.0f);
}

}  // namespace

Timbre timbrePreset(int idx) {
  Timbre t;
  switch (idx) {
    default:
    case 0:  // PURE — clean tone with a little body
      t.ratio[0] = 1; t.ratio[1] = 2; t.ratio[2] = 3; t.ratio[3] = 4;
      t.gain[0] = 0.9f; t.gain[1] = 0.12f; t.gain[2] = 0.05f; t.gain[3] = 0.02f;
      t.detuneCents = 0.3f;
      t.shimmer = 0.7f;
      t.attack = 0.05f; t.decay = 0.3f; t.sustain = 0.85f; t.release = 0.6f;
      t.outputTrim = 1.000f;
      break;
    case 1:  // ORGAN — octaves, pronounced beating, bowed-string envelope
      t.ratio[0] = 1; t.ratio[1] = 2; t.ratio[2] = 4; t.ratio[3] = 8;
      t.gain[0] = 0.7f; t.gain[1] = 0.35f; t.gain[2] = 0.18f; t.gain[3] = 0.08f;
      t.detuneCents = 0.9f;
      t.shimmer = 1.5f;  // drones live off the breathing
      t.attack = 0.8f; t.decay = 0.5f; t.sustain = 0.9f; t.release = 2.0f;
      t.outputTrim = 0.577f;
      break;
    case 2:  // REED — odd harmonics, clarinet-like
      t.ratio[0] = 1; t.ratio[1] = 3; t.ratio[2] = 5; t.ratio[3] = 7;
      t.gain[0] = 0.8f; t.gain[1] = 0.25f; t.gain[2] = 0.12f; t.gain[3] = 0.06f;
      t.detuneCents = 0.2f;
      t.shimmer = 0.5f;
      t.attack = 0.02f; t.decay = 0.2f; t.sustain = 0.8f; t.release = 0.3f;
      t.outputTrim = 0.629f;
      break;
    case 3:  // CHIME — bright music-box ping, uppers melt into a pure tone
      t.ratio[0] = 1; t.ratio[1] = 2; t.ratio[2] = 3; t.ratio[3] = 4;
      t.gain[0] = 0.8f; t.gain[1] = 0.35f; t.gain[2] = 0.26f; t.gain[3] = 0.16f;
      // uppers linger much longer — the melt stays, the gloom goes
      t.pdecay[0] = 0.0f; t.pdecay[1] = 2.5f; t.pdecay[2] = 1.0f;
      t.pdecay[3] = 0.5f;
      t.detuneCents = 0.25f;
      t.shimmer = 0.9f;
      t.attack = 0.003f; t.decay = 0.5f; t.sustain = 0.55f; t.release = 2.8f;
      t.outputTrim = 0.560f;
      break;
    case 4:  // MUSICBOX — a plucked tine: bright sparkle, no sustain, the
             // note ends by itself (sustain 0 -> the voice frees its slot)
      t.ratio[0] = 1; t.ratio[1] = 2; t.ratio[2] = 4; t.ratio[3] = 6;
      t.gain[0] = 0.7f; t.gain[1] = 0.4f; t.gain[2] = 0.3f; t.gain[3] = 0.18f;
      t.pdecay[0] = 1.2f; t.pdecay[1] = 0.6f; t.pdecay[2] = 0.25f;
      t.pdecay[3] = 0.12f;
      t.detuneCents = 0.3f;
      t.shimmer = 0.4f;
      t.attack = 0.001f; t.decay = 0.9f; t.sustain = 0.0f; t.release = 0.6f;
      t.outputTrim = 0.743f;
      break;
    case 5:  // WARM — broad but gentle: a detuned-pair subtractive bloom
      t.oscillator = OscillatorKind::FatSaw;
      t.oscGain = 0.55f;
      t.ratio[0] = t.ratio[1] = t.ratio[2] = t.ratio[3] = 1.0f;
      t.detuneCents = 6.0f;
      t.shimmer = 0.0f;
      t.attack = 0.012f; t.decay = 0.32f; t.sustain = 0.68f; t.release = 1.1f;
      t.filterMode = FilterMode::Lowpass;
      t.filterScale = 0.18f;
      t.filterRes = 0.27f;
      t.filterEnvOct = 2.1f;
      t.filterEnvAttack = 0.004f;
      t.filterEnvDecay = 0.38f;
      t.filterKeyTrack = 0.6f;
      t.drive = 1.45f;
      t.attackNoise = 0.012f;
      t.noiseDecay = 0.018f;
      t.outputTrim = 0.543f;
      t.chorusDepth = 0.18f;
      break;
    case 6:  // HOLLOW — a slowly breathing pulse, kept soft-edged
      // The hollow, nasal colour is the pulse width and its slow PWM, not the
      // filter. A notch was the wrong tool to hold it: notch = input minus a
      // sliver, so a square's whole harmonic ladder reached the speaker and
      // the top of the keyboard rasped. A key-tracked lowpass keeps roughly
      // the same handful of harmonics in every register instead.
      t.oscillator = OscillatorKind::Pulse;
      t.oscGain = 0.66f;
      t.pulseWidth = 0.47f;
      t.pwmDepth = 0.22f;
      t.pwmRateHz = 0.32f;
      t.detuneCents = 0.0f;
      t.shimmer = 0.0f;
      t.attack = 0.025f; t.decay = 0.28f; t.sustain = 0.72f; t.release = 0.85f;
      t.filterMode = FilterMode::Lowpass;
      t.filterScale = 0.16f;
      t.filterRes = 0.38f;
      t.filterEnvOct = 0.70f;
      t.filterEnvAttack = 0.010f;
      t.filterEnvDecay = 0.62f;
      t.filterKeyTrack = 0.6f;
      t.drive = 1.28f;
      t.attackNoise = 0.006f;
      t.noiseDecay = 0.014f;
      t.outputTrim = 0.400f;
      t.chorusDepth = 0.12f;
      break;
  }
  return t;
}

uint32_t Voice::rng() {
  rng_ ^= rng_ << 13;
  rng_ ^= rng_ >> 17;
  rng_ ^= rng_ << 5;
  return rng_;
}

float Voice::rnd11() {
  return (float)(int32_t)rng() * (1.0f / 2147483648.0f);
}

void Voice::init(float sampleRate, uint32_t seed) {
  sr_ = sampleRate;
  env_.init(sampleRate);
  rng_ = seed * 2654435761u + 1u;
  pwmPhase_ = (float)(rng() >> 8) * (1.0f / 16777216.0f);
  id_ = -1;
  persistent_ = false;
  for (int k = 0; k < Timbre::kPartials; ++k) {
    phase_[k] = rng();  // random start phases — no phase-aligned clicks
    ratio_[k] = 1.0f;
    gain_[k] = 0.0f;
    det_[k] = 0.0f;
  }
}

void Voice::noteOn(int32_t id, float cents, float vel, const Timbre& t,
                   float glideSec, uint32_t age, bool persistent,
                   bool hasGlideFrom, float glideFromCents) {
  const bool wasActive = env_.active();
  id_ = id;
  vel_ = clampf(vel, 0.0f, 1.0f);
  age_ = age;
  persistent_ = persistent;
  oscillator_ = t.oscillator;
  oscGain_ = t.oscGain;
  pulseWidth_ = clampf(t.pulseWidth, 0.08f, 0.92f);
  pwmDepth_ = clampf(t.pwmDepth, 0.0f, 0.40f);
  pwmRateHz_ = clampf(t.pwmRateHz, 0.02f, 4.0f);
  noiseAmp_ = clampf(t.attackNoise, 0.0f, 0.35f);
  trim_ = clampf(t.outputTrim, 0.05f, 8.0f);
  noiseDecay_ = clampf(t.noiseDecay, 0.002f, 0.5f);
  pulseWidthCurrent_ = clampf(
      pulseWidth_ + pwmDepth_ * sinTurns(pwmPhase_), 0.08f, 0.92f);
  for (int k = 0; k < Timbre::kPartials; ++k) {
    ratio_[k] = t.ratio[k];
    gain_[k] = t.gain[k];
    det_[k] = (k == 0) ? 0.0f : rnd11() * t.detuneCents;
    pdecay_[k] = t.pdecay[k];
    detRatio_[k] = 1.0f;  // filled in below, once det_ is final
    pamp_[k] = 1.0f;
    // breathing LFOs: each partial gets its own ultra-slow rate (0.03-0.15
    // Hz) and phase — no two notes, and no two partials, ever breathe alike
    lfoPhase_[k] = (float)(rng() >> 8) * (1.0f / 16777216.0f);
    lfoInc_[k] = (0.03f + 0.12f * (float)(rng() >> 8) * (1.0f / 16777216.0f)) / sr_;
  }
  shimmer_ = t.shimmer;
  detLimit_ = t.detuneCents;
  if (oscillator_ == OscillatorKind::FatSaw) {
    // A symmetrical detuned PAIR, not a trio: the third saw cost a third of
    // this timbre's CPU and, sitting dead centre, contributed the least
    // motion. The phases stay free-running, so repeated notes still never
    // phase-lock into a click.
    det_[0] = -t.detuneCents;
    det_[1] = t.detuneCents;
    det_[2] = 0.0f;
    det_[3] = 0.0f;
  }
  // Virtual pitch needs the target partials to IMPLY f0: coprime integer
  // ratios (2,3 / 3,5) leave a residue pitch at f0, but even-only stacks
  // (ORGAN and MUSICBOX use 2,4) share a factor of 2 and the ear rebuilds
  // the OCTAVE instead — worse than losing the fundamental. Skip those.
  {
    const int r1 = (int)(t.ratio[1] + 0.5f);
    const int r2 = (int)(t.ratio[2] + 0.5f);
    const bool ints = fabsf(t.ratio[1] - (float)r1) < 0.01f &&
                      fabsf(t.ratio[2] - (float)r2) < 0.01f;
    int a = r1 > 0 ? r1 : 1, b = r2 > 0 ? r2 : 1;
    while (b) {
      const int r = a % b;
      a = b;
      b = r;
    }
    bassOk_ = oscillator_ == OscillatorKind::Additive && ints && a == 1;
  }
  for (int k = 0; k < Timbre::kPartials; ++k)
    detRatio_[k] = det_[k] == 0.0f ? 1.0f : centsToRatio(det_[k]);
  decayN_ = 0;      // rebuild the per-block decay multipliers
  ratioCents_ = 1e30f;  // and the pitch ratio

  env_.setTimes(t.attack, t.decay, t.sustain, t.release);
  targetCents_ = cents;
  glideSec_ = glideSec;
  if (hasGlideFrom && glideSec > 0.0f)
    cents_ = glideFromCents;
  else if (!wasActive || glideSec <= 0.0f)
    cents_ = cents;
  env_.noteOn();
}

void Voice::render(float* out, int n, float baseHz, float bendRatio,
                   float bassVoicing) {
  if (!env_.active() || n <= 0) return;

  if (glideSec_ > 0.0001f) {
    const float c = 1.0f - expf(-(float)n * 4.0f / (sr_ * glideSec_));
    cents_ += c * (targetCents_ - cents_);
  } else {
    cents_ = targetCents_;
  }

  // Every exp/log below used to be recomputed each block from arguments that
  // had not changed. On a chip whose transcendentals are software routines
  // that was the single most expensive thing a silent, steady voice did.
  if (cents_ != ratioCents_) {
    ratioCents_ = cents_;
    centsRatio_ = centsToRatio(cents_);
  }
  if (n != decayN_) {
    decayN_ = n;
    for (int k = 0; k < Timbre::kPartials; ++k)
      pdecayMul_[k] = pdecay_[k] > 0.0f
                          ? expf(-(float)n / (sr_ * pdecay_[k]))
                          : 1.0f;
    noiseMul_ = expf(-(float)n / (sr_ * noiseDecay_));
  }

  // Only the partials this oscillator actually reads are worth preparing.
  const int used = oscillator_ == OscillatorKind::Additive
                       ? Timbre::kPartials
                       : (oscillator_ == OscillatorKind::FatSaw
                              ? kFatSawCount
                              : 1);
  const float f0 = baseHz * centsRatio_ * bendRatio;
  const float invSr = 1.0f / sr_;
  uint32_t inc[Timbre::kPartials] = {0, 0, 0, 0};
  float g[Timbre::kPartials] = {0.0f, 0.0f, 0.0f, 0.0f};
  for (int k = 0; k < used; ++k) {
    const float ph = f0 * ratio_[k] * detRatio_[k] * invSr;
    // Mute partials above ~Nyquist and skip the conversion entirely:
    // (uint32_t)(ph * 2^32) with ph >= 1.0 would be undefined behavior.
    // !(ph > 0) also catches NaN.
    if (!(ph > 0.0f) || ph >= 0.45f) {
      g[k] = 0.0f;
      inc[k] = 0;
    } else {
      // breathing: slow independent amplitude wobble per partial —
      // fundamental barely, uppers more; a held tone never sits still
      const float depth = shimmer_ * (k == 0 ? 0.05f : 0.14f + 0.06f * (float)k);
      const float breathe = 1.0f + depth * sinTurns(lfoPhase_[k]);
      g[k] = gain_[k] * pamp_[k] * breathe;
      // 4294967296.0f, not 4294967296.0: this chip has no double FPU, so a
      // bare double literal turned one multiply into three software-emulated
      // library calls, per partial, per block. The value is a power of two
      // and ph < 0.45, so the float multiply is exact — same bits, no calls.
      inc[k] = (uint32_t)(ph * 4294967296.0f);
    }
  }
  // Psychoacoustic bass: the additive engine KNOWS f0, so no mix-side
  // processor is needed — below ~250 Hz shift the fundamental's energy
  // into partials 2-3 (harmonics of f0 in every preset) and the ear
  // reconstructs the missing fundamental. The note sounds AT its pitch
  // without asking a 3 cm membrane for air it cannot move.
  if (bassVoicing > 0.0f && bassOk_ && f0 < 250.0f) {
    const float vb =
        fminf(1.0f, (250.0f - f0) * (1.0f / 150.0f)) * bassVoicing;
    const float moved = g[0] * 0.75f * vb;
    g[0] -= moved;
    if (inc[1]) g[1] += moved * 0.6f;
    if (inc[2]) g[2] += moved * 0.4f;
  }
  // block-rate evolution: melt, LFO phases, wandering detune
  const bool drifts =
      oscillator_ == OscillatorKind::Additive && detLimit_ > 0.0f;
  for (int k = 0; k < used; ++k) {
    if (pdecay_[k] > 0.0f) pamp_[k] *= pdecayMul_[k];
    lfoPhase_[k] += lfoInc_[k] * (float)n;
    if (lfoPhase_[k] >= 1.0f) lfoPhase_[k] -= 1.0f;
    if (drifts && k > 0) {
      // detune drifts, so the beating tempo itself slowly evolves
      det_[k] += rnd11() * 0.012f;
      if (det_[k] > detLimit_) det_[k] = 2.0f * detLimit_ - det_[k];
      if (det_[k] < -detLimit_) det_[k] = -2.0f * detLimit_ - det_[k];
      detRatio_[k] = centsToRatio(det_[k]);
    }
  }

  float pulseWidth = pulseWidthCurrent_;
  float pulseWidthStep = 0.0f;
  if (oscillator_ == OscillatorKind::Pulse) {
    pwmPhase_ += pwmRateHz_ * (float)n / sr_;
    if (pwmPhase_ >= 1.0f) pwmPhase_ -= floorf(pwmPhase_);
    const float targetWidth = clampf(
        pulseWidth_ + pwmDepth_ * sinTurns(pwmPhase_), 0.08f, 0.92f);
    // PWM is evaluated cheaply once per block but interpolated per sample;
    // there is no 375 Hz staircase creating extra, un-bandlimited edges.
    pulseWidthStep = (targetWidth - pulseWidth) / (float)n;
    pulseWidthCurrent_ = targetWidth;
  }

  // One loop per oscillator kind rather than a switch per sample: on the
  // ESP32 the audio task has no room for per-sample dispatch, and every
  // block-invariant term below (dt, its reciprocal, the noise level) used to
  // be recomputed 48000 times a second per voice.
  const float noiseAmp = noiseAmp_;
  const bool useNoise = noiseAmp > 0.0001f;
  // vel_ is a member, and the compiler must assume the store to out[i] can
  // reach it, so it reloaded velocity from memory on every single sample.
  const float vel = vel_ * trim_;

  switch (oscillator_) {
    case OscillatorKind::FatSaw: {
      // Two existing partial phases become a detuned saw pair. PolyBLEP is
      // cheaper here than four interpolated sine partials and consumes no
      // extra wavetable RAM.
      float dt[kFatSawCount], invDt[kFatSawCount];
      for (int k = 0; k < kFatSawCount; ++k) {
        dt[k] = (float)inc[k] * kPhaseToTurns;
        invDt[k] = dt[k] > 0.0f ? 1.0f / dt[k] : 0.0f;
      }
      // Phases live in registers for the whole block: phase_[] is a member,
      // so writing out[i] forced a reload and a store-back per oscillator per
      // sample. Same order, same arithmetic — only the traffic goes away.
      uint32_t ph0 = phase_[0], ph1 = phase_[1];
      const uint32_t in0 = inc[0], in1 = inc[1];
      const float dt0 = dt[0], dt1 = dt[1];
      const float iv0 = invDt[0], iv1 = invDt[1];
      const float oscGain = oscGain_;
      for (int i = 0; i < n; ++i) {
        const float e = env_.process();
        float s = 0.0f;
        if (in0) s += bandSaw(ph0, dt0, iv0);
        ph0 += in0;
        if (in1) s += bandSaw(ph1, dt1, iv1);
        ph1 += in1;
        s *= oscGain;
        if (useNoise) s += rnd11() * noiseAmp;
        out[i] += s * vel * e;
      }
      phase_[0] = ph0;
      phase_[1] = ph1;
      break;
    }
    case OscillatorKind::Pulse: {
      const float dt = (float)inc[0] * kPhaseToTurns;
      const float invDt = dt > 0.0f ? 1.0f / dt : 0.0f;
      uint32_t ph0 = phase_[0];
      const uint32_t in0 = inc[0];
      const float oscGain = oscGain_;
      for (int i = 0; i < n; ++i) {
        const float e = env_.process();
        float s = 0.0f;
        if (in0) s = bandPulse(ph0, dt, invDt, pulseWidth) * oscGain;
        ph0 += in0;
        pulseWidth += pulseWidthStep;
        if (useNoise) s += rnd11() * noiseAmp;
        out[i] += s * vel * e;
      }
      phase_[0] = ph0;
      break;
    }
    case OscillatorKind::Additive:
    default: {
      // Scalarised for the same reason as the saw pair above, and because a
      // constexpr trip count of four was still being emitted as a hardware
      // loop: four independent partial chains cannot overlap in the pipeline
      // while they are stuck behind a loop back-edge. Accumulation starts at
      // 0.0f and runs k = 0..3 exactly as before, so the sum is bit-identical.
      const float* tab = sineTable().t;
      uint32_t ph0 = phase_[0], ph1 = phase_[1];
      uint32_t ph2 = phase_[2], ph3 = phase_[3];
      const uint32_t in0 = inc[0], in1 = inc[1], in2 = inc[2], in3 = inc[3];
      const float g0 = g[0], g1 = g[1], g2 = g[2], g3 = g[3];
      for (int i = 0; i < n; ++i) {
        const float e = env_.process();
        ph0 += in0;
        ph1 += in1;
        ph2 += in2;
        ph3 += in3;
        float s = 0.0f;
        s += g0 * sineAtTab(tab, ph0);
        s += g1 * sineAtTab(tab, ph1);
        s += g2 * sineAtTab(tab, ph2);
        s += g3 * sineAtTab(tab, ph3);
        if (useNoise) s += rnd11() * noiseAmp;
        out[i] += s * vel * e;
      }
      phase_[0] = ph0;
      phase_[1] = ph1;
      phase_[2] = ph2;
      phase_[3] = ph3;
      break;
    }
  }
  if (useNoise) noiseAmp_ *= noiseMul_;
}

}  // namespace ga
