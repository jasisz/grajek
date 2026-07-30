#include "firefly.h"

#include <Arduino.h>
#include <M5Unified.h>
#include <math.h>

#include "hal/board_pins.h"

namespace {

// current light, as float RGB 0..1 (decays exponentially toward dark)
float s_r = 0.0f, s_g = 0.0f, s_b = 0.0f;
float s_decayPerSec = 6.0f;  // flash decay; ghosts set a slower one
uint32_t s_lastTickMs = 0;
uint32_t s_lastWriteMs = 0;
uint8_t s_wr = 0, s_wg = 0, s_wb = 0;  // last written value (skip no-ops)
bool s_dark = true;

// battery dusk: cached level, sampled rarely (ADC + math are not free)
float s_dusk = 1.0f;
uint32_t s_lastBattMs = 0;

// pitch -> hue: the same octave fold as the screen's X axis, so the same
// note is always the same color, one octave up lands on the same hue
void pitchColor(float cents, float& r, float& g, float& b) {
  float oct = cents / 1200.0f;
  float h = (oct - floorf(oct)) * 6.0f;  // hue sector 0..6
  const float c = 1.0f;
  const float x = 1.0f - fabsf(fmodf(h, 2.0f) - 1.0f);
  switch ((int)h) {
    case 0: r = c; g = x; b = 0; break;
    case 1: r = x; g = c; b = 0; break;
    case 2: r = 0; g = c; b = x; break;
    case 3: r = 0; g = x; b = c; break;
    case 4: r = x; g = 0; b = c; break;
    default: r = c; g = 0; b = x; break;
  }
}

void show(float master) {
  // perceptual-ish curve + a hard cap: this is a firefly, not a flashlight
  const float k = master * s_dusk * 0.55f;
  const uint8_t r = (uint8_t)(255.0f * s_r * s_r * k);
  const uint8_t g = (uint8_t)(255.0f * s_g * s_g * k);
  const uint8_t b = (uint8_t)(255.0f * s_b * s_b * k);
  if (r == s_wr && g == s_wg && b == s_wb) return;
  s_wr = r; s_wg = g; s_wb = b;
  rgbLedWrite((uint8_t)hal::kPinRgbLed, r, g, b);
}

}  // namespace

namespace firefly {

void note(float cents, float vel) {
  float r, g, b;
  pitchColor(cents, r, g, b);
  const float v = 0.35f + 0.65f * (vel < 0.0f ? 0.0f : (vel > 1.0f ? 1.0f : vel));
  // a new note wins over whatever was fading — flashes never sum to white
  s_r = r * v;
  s_g = g * v;
  s_b = b * v;
  s_decayPerSec = 6.0f;  // ~300 ms to embers
  s_dark = false;
}

void ghost(float cents) {
  float r, g, b;
  pitchColor(cents, r, g, b);
  s_r = r * 0.45f;
  s_g = g * 0.45f;
  s_b = b * 0.45f;
  s_decayPerSec = 1.8f;  // ~1.2 s afterglow — a memory, not an event
  s_dark = false;
}

void tick() {
  const uint32_t now = millis();
  if (now - s_lastTickMs < 20) return;  // ~50 Hz is plenty for one LED
  const float dt = (float)(now - s_lastTickMs) * 0.001f;
  s_lastTickMs = now;

  // battery dusk, sampled every 5 s: full glow above 40%, fading to a
  // quarter at 10% — the box grows sleepy, it never asks for anything
  if (now - s_lastBattMs > 5000) {
    s_lastBattMs = now;
    const int lvl = M5.Power.getBatteryLevel();  // -1 when unknown
    if (lvl >= 0)
      s_dusk = 0.25f + 0.75f * fminf(1.0f, fmaxf(0.0f, (lvl - 10) / 30.0f));
  }

  if (s_dark) return;
  const float k = expf(-s_decayPerSec * dt);
  s_r *= k;
  s_g *= k;
  s_b *= k;
  if (s_r + s_g + s_b < 0.01f) {  // fully out: write black once, then rest
    s_r = s_g = s_b = 0.0f;
    s_dark = true;
  }
  show(1.0f);
}

}  // namespace firefly
