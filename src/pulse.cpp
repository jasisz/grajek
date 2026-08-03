#include "pulse.h"

#include <Arduino.h>

#include "gk_pulse.h"

namespace {

constexpr int32_t kPulseId = 1300;    // outside grid/background/ghosts/chimes
constexpr int kMusicboxPreset = 4;    // the heart always beats MUSICBOX

// Double on purpose: per-onset and per-beat math only (no audio-rate cost),
// and float seconds lose millisecond precision after ~2 hours of uptime.
// Extend millis() so the beat clock never jumps backwards at 49.7 days.
uint64_t s_clockHighMs = 0;
uint32_t s_clockLastMs = 0;
bool s_clockStarted = false;

double extendedNowSec() {
  const uint32_t ms = millis();
  if (s_clockStarted && ms < s_clockLastMs) s_clockHighMs += 1ull << 32;
  s_clockStarted = true;
  s_clockLastMs = ms;
  return (double)(s_clockHighMs + ms) * 0.001;
}

gk::PulseTracker s_pulse;
ga::Engine* s_engine = nullptr;

}  // namespace

namespace pulse {

void init(ga::Engine* engine) { s_engine = engine; }

double nowSec() { return extendedNowSec(); }

bool ticking() { return s_pulse.ticking(); }
double memoryPeriodSec() { return s_pulse.memoryPeriodSec(); }
double lastOnsetSec() { return s_pulse.lastOnsetSec(); }
void restoreMemory(double periodSec) { s_pulse.restoreMemory(periodSec); }

void onOnset() { s_pulse.onOnset(extendedNowSec()); }

void tick(bool suspended, const float* chordCents, int chordCount,
          int playerPreset) {
  const double now = extendedNowSec();  // maintain the high word every pass
  if (!s_engine) return;
  if (suspended) {
    // Goodnight owns the whole box. A fresh rhythm will summon the heart
    // after waking; even a half-learned pre-sleep rhythm must not combine
    // with tomorrow's first presses.
    if (s_pulse.suspend()) s_engine->noteOff(kPulseId);
    return;
  }
  if (chordCount < 0) chordCount = 0;
  if (chordCount > 0 && !chordCents) chordCount = 0;
  const gk::PulseBeat beat = s_pulse.tick(now, chordCount);
  if (!beat.play) return;
  const float cents = beat.chordIndex >= 0 ? chordCents[beat.chordIndex] : 0.0f;
  // MUSICBOX ends by itself — no note-off bookkeeping; the FIFO preset
  // sandwich is race-free (single producer, queue drained per render)
  s_engine->setParam(ga::Param::TimbrePreset, (float)kMusicboxPreset);
  s_engine->noteOn(kPulseId, cents, beat.velocity);
  s_engine->setParam(ga::Param::TimbrePreset, (float)playerPreset);
}

}  // namespace pulse
