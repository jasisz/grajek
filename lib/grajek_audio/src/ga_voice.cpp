#include "ga_voice.h"

namespace ga {

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
      break;
    case 1:  // DRONE — octaves, pronounced beating, bowed-string envelope
      t.ratio[0] = 1; t.ratio[1] = 2; t.ratio[2] = 4; t.ratio[3] = 8;
      t.gain[0] = 0.7f; t.gain[1] = 0.35f; t.gain[2] = 0.18f; t.gain[3] = 0.08f;
      t.detuneCents = 0.9f;
      t.shimmer = 1.5f;  // drones live off the breathing
      t.attack = 0.8f; t.decay = 0.5f; t.sustain = 0.9f; t.release = 2.0f;
      break;
    case 2:  // REED — odd harmonics, clarinet-like
      t.ratio[0] = 1; t.ratio[1] = 3; t.ratio[2] = 5; t.ratio[3] = 7;
      t.gain[0] = 0.8f; t.gain[1] = 0.25f; t.gain[2] = 0.12f; t.gain[3] = 0.06f;
      t.detuneCents = 0.2f;
      t.shimmer = 0.5f;
      t.attack = 0.02f; t.decay = 0.2f; t.sustain = 0.8f; t.release = 0.3f;
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
  id_ = -1;
  for (int k = 0; k < Timbre::kPartials; ++k) {
    phase_[k] = rng();  // random start phases — no phase-aligned clicks
    ratio_[k] = 1.0f;
    gain_[k] = 0.0f;
    det_[k] = 0.0f;
  }
}

void Voice::noteOn(int32_t id, float cents, float vel, const Timbre& t,
                   float glideSec, uint32_t age) {
  const bool wasActive = env_.active();
  id_ = id;
  vel_ = clampf(vel, 0.0f, 1.0f);
  age_ = age;
  for (int k = 0; k < Timbre::kPartials; ++k) {
    ratio_[k] = t.ratio[k];
    gain_[k] = t.gain[k];
    det_[k] = (k == 0) ? 0.0f : rnd11() * t.detuneCents;
    pdecay_[k] = t.pdecay[k];
    pamp_[k] = 1.0f;
    // breathing LFOs: each partial gets its own ultra-slow rate (0.03-0.15
    // Hz) and phase — no two notes, and no two partials, ever breathe alike
    lfoPhase_[k] = (float)(rng() >> 8) * (1.0f / 16777216.0f);
    lfoInc_[k] = (0.03f + 0.12f * (float)(rng() >> 8) * (1.0f / 16777216.0f)) / sr_;
  }
  shimmer_ = t.shimmer;
  detLimit_ = t.detuneCents;
  env_.setTimes(t.attack, t.decay, t.sustain, t.release);
  targetCents_ = cents;
  glideSec_ = glideSec;
  if (!wasActive || glideSec <= 0.0f) cents_ = cents;
  env_.noteOn();
}

void Voice::render(float* out, int n, float baseHz, float bendRatio,
                   bool bassVoicing) {
  if (!env_.active() || n <= 0) return;

  if (glideSec_ > 0.0001f) {
    const float c = 1.0f - expf(-(float)n * 4.0f / (sr_ * glideSec_));
    cents_ += c * (targetCents_ - cents_);
  } else {
    cents_ = targetCents_;
  }

  const float f0 = baseHz * centsToRatio(cents_) * bendRatio;
  uint32_t inc[Timbre::kPartials];
  float g[Timbre::kPartials];
  for (int k = 0; k < Timbre::kPartials; ++k) {
    const float ph = f0 * ratio_[k] * centsToRatio(det_[k]) / sr_;
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
      inc[k] = (uint32_t)(ph * 4294967296.0);
    }
  }
  // Psychoacoustic bass: the additive engine KNOWS f0, so no mix-side
  // processor is needed — below ~250 Hz shift the fundamental's energy
  // into partials 2-3 (harmonics of f0 in every preset) and the ear
  // reconstructs the missing fundamental. The note sounds AT its pitch
  // without asking a 3 cm membrane for air it cannot move.
  if (bassVoicing && f0 < 250.0f) {
    const float vb = fminf(1.0f, (250.0f - f0) * (1.0f / 150.0f));
    const float moved = g[0] * 0.75f * vb;
    g[0] -= moved;
    if (inc[1]) g[1] += moved * 0.6f;
    if (inc[2]) g[2] += moved * 0.4f;
  }
  // block-rate evolution: melt, LFO phases, wandering detune
  for (int k = 0; k < Timbre::kPartials; ++k) {
    if (pdecay_[k] > 0.0f)
      pamp_[k] *= expf(-(float)n / (sr_ * pdecay_[k]));
    lfoPhase_[k] += lfoInc_[k] * (float)n;
    if (lfoPhase_[k] >= 1.0f) lfoPhase_[k] -= 1.0f;
    if (k > 0 && detLimit_ > 0.0f) {
      // detune drifts, so the beating tempo itself slowly evolves
      det_[k] += rnd11() * 0.012f;
      if (det_[k] > detLimit_) det_[k] = 2.0f * detLimit_ - det_[k];
      if (det_[k] < -detLimit_) det_[k] = -2.0f * detLimit_ - det_[k];
    }
  }

  const float* tab = sineTable().t;  // hoisted out of the sample loop
  for (int i = 0; i < n; ++i) {
    const float e = env_.process();
    float s = 0.0f;
    for (int k = 0; k < Timbre::kPartials; ++k) {
      phase_[k] += inc[k];
      s += g[k] * sineAtTab(tab, phase_[k]);
    }
    out[i] += s * vel_ * e;
  }
}

}  // namespace ga
