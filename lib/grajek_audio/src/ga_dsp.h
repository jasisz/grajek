// grajek_audio — shared DSP primitives.
// Pure C++17, no Arduino/ESP32 dependencies. Buffers and math only.
#pragma once
#include <stdint.h>
#include <math.h>

namespace ga {

constexpr float kPi  = 3.14159265358979f;
constexpr float kTau = 6.28318530717959f;

// Sine table: 2048 samples + one guard sample for linear interpolation.
constexpr int kSineTableBits = 11;
constexpr int kSineTableSize = 1 << kSineTableBits;

struct SineTable {
  float t[kSineTableSize + 1];
  SineTable() {
    for (int i = 0; i <= kSineTableSize; ++i)
      t[i] = sinf(kTau * (float)i / (float)kSineTableSize);
  }
};

// Lazy init — Engine::init() calls this before the audio thread starts.
const SineTable& sineTable();

// Phase spans the full uint32 range (2^32 = one period), linear interpolation.
// Hot-path variant: pass the table pointer hoisted out of the sample loop —
// the cross-TU sineTable() call cannot be inlined by the compiler.
inline float sineAtTab(const float* tab, uint32_t phase) {
  const uint32_t idx = phase >> (32 - kSineTableBits);
  constexpr uint32_t fracMask = (1u << (32 - kSineTableBits)) - 1u;
  constexpr float fracScale = 1.0f / (float)(1u << (32 - kSineTableBits));
  const float frac = (float)(phase & fracMask) * fracScale;
  const float a = tab[idx];
  const float b = tab[idx + 1];
  return a + (b - a) * frac;
}

inline float sineAt(uint32_t phase) { return sineAtTab(sineTable().t, phase); }

// Cheap sine for LFOs: phase in turns [0,1), parabola approximation (~6%
// error — irrelevant below 1 Hz). No tables, no libm on the hot path.
inline float sinTurns(float t) {
  t -= (float)(int)t;
  if (t < 0.0f) t += 1.0f;
  return t < 0.5f ? 16.0f * t * (0.5f - t)
                  : -16.0f * (t - 0.5f) * (1.0f - t);
}

// Cubic soft clip — gentle saturation instead of a hard edge.
inline float softClip(float x) {
  if (x >  1.5f) return  1.0f;
  if (x < -1.5f) return -1.0f;
  return x - (4.0f / 27.0f) * x * x * x;
}

inline float centsToRatio(float cents) { return exp2f(cents * (1.0f / 1200.0f)); }

inline float clampf(float v, float lo, float hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}

}  // namespace ga
