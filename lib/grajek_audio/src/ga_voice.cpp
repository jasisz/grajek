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
      t.attack = 0.05f; t.decay = 0.3f; t.sustain = 0.85f; t.release = 0.6f;
      break;
    case 1:  // DRONE — octaves, pronounced beating, bowed-string envelope
      t.ratio[0] = 1; t.ratio[1] = 2; t.ratio[2] = 4; t.ratio[3] = 8;
      t.gain[0] = 0.7f; t.gain[1] = 0.35f; t.gain[2] = 0.18f; t.gain[3] = 0.08f;
      t.detuneCents = 0.9f;
      t.attack = 0.8f; t.decay = 0.5f; t.sustain = 0.9f; t.release = 2.0f;
      break;
    case 2:  // REED — odd harmonics, clarinet-like
      t.ratio[0] = 1; t.ratio[1] = 3; t.ratio[2] = 5; t.ratio[3] = 7;
      t.gain[0] = 0.8f; t.gain[1] = 0.25f; t.gain[2] = 0.12f; t.gain[3] = 0.06f;
      t.detuneCents = 0.2f;
      t.attack = 0.02f; t.decay = 0.2f; t.sustain = 0.8f; t.release = 0.3f;
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
  }
  env_.setTimes(t.attack, t.decay, t.sustain, t.release);
  targetCents_ = cents;
  glideSec_ = glideSec;
  if (!wasActive || glideSec <= 0.0f) cents_ = cents;
  env_.noteOn();
}

void Voice::render(float* out, int n, float baseHz, float bendRatio) {
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
      g[k] = gain_[k];
      inc[k] = (uint32_t)(ph * 4294967296.0);
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
