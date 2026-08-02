#include "pulse.h"

#include <Arduino.h>
#include <math.h>
#include <string.h>

#include "ambient.h"
#include "settings.h"

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

struct Pulse {
  double onsets[8];
  int count = 0;
  double period = 0.0;
  double nextBeatAt = 0.0;
  double lastOnset = 0.0;
  int beatsAlone = 0;
  int beatsPlayed = 0;  // warm-up: the heart fades in over its first beats
  int step = 0;         // position in the background-chord arpeggio
  bool ticking = false;
  double memory = 0.0;  // last stable period — the ghosts sow on this grid
};
Pulse s_pulse;
ga::Engine* s_engine = nullptr;

}  // namespace

namespace pulse {

void init(ga::Engine* engine) { s_engine = engine; }

double nowSec() { return extendedNowSec(); }

bool ticking() { return s_pulse.ticking; }
double memoryPeriodSec() { return s_pulse.memory; }
double lastOnsetSec() { return s_pulse.lastOnset; }
void restoreMemory(double periodSec) { s_pulse.memory = periodSec; }

void onOnset() {
  const double t = extendedNowSec();
  Pulse& p = s_pulse;
  const double d = p.lastOnset > 0.0 ? t - p.lastOnset : 0.0;
  if (p.count < 8) {
    p.onsets[p.count++] = t;
  } else {
    memmove(p.onsets, p.onsets + 1, 7 * sizeof(double));
    p.onsets[7] = t;
  }
  p.lastOnset = t;

  if (p.ticking) {
    // ENTRAINED: a phase-locked loop, not a pass/fail test — the heart
    // bends toward the player instead of giving up.
    p.beatsAlone = 0;
    const double r = p.period > 0.0 ? d / p.period : 0.0;
    if (r > 0.7 && r < 1.4) {
      p.period += 0.25 * (d - p.period);        // tempo follows you
    } else if (r > 1.6 && r < 2.4) {
      p.period += 0.12 * (d * 0.5 - p.period);  // you moved to half notes
    } else if (r > 0.35 && r < 0.65) {
      p.period += 0.12 * (d * 2.0 - p.period);  // you moved to eighths
    }
    p.memory = p.period;
    // phase: nudge the beat grid halfway toward this onset
    double e = t - p.nextBeatAt;
    e -= p.period * round(e / p.period);  // wrapped to [-T/2, T/2]
    p.nextBeatAt += 0.5 * e;
    while (p.nextBeatAt <= t) p.nextBeatAt += p.period;
    return;
  }

  // bootstrap: a few even intervals summon the heart
  double ioi[7];
  int n = 0;
  for (int i = 1; i < p.count; ++i) {
    const double dd = p.onsets[i] - p.onsets[i - 1];
    if (dd > 0.18 && dd < 2.0) ioi[n++] = dd;
  }
  if (n < 3) return;
  const int m = n < 5 ? n : 5;  // judge the last few intervals
  double w[5];
  for (int i = 0; i < m; ++i) w[i] = ioi[n - m + i];
  for (int i = 1; i < m; ++i) {  // insertion sort
    const double v = w[i];
    int j = i;
    while (j > 0 && w[j - 1] > v) {
      w[j] = w[j - 1];
      --j;
    }
    w[j] = v;
  }
  const double med = w[m / 2];
  int good = 0;
  for (int i = 0; i < m; ++i)
    if (fabs(w[i] - med) < 0.15 * med) ++good;
  if (good >= m - 1) {
    p.period = med;
    p.memory = med;
    p.nextBeatAt = t + p.period;  // the beat anchors to YOUR press
    p.beatsAlone = 0;
    p.beatsPlayed = 0;
    p.step = 0;
    p.ticking = true;
  }
}

void tick() {
  Pulse& p = s_pulse;
  const double now = extendedNowSec();  // maintain the high word every pass
  if (!s_engine) return;
  if (ambient::lullabyActive()) {
    // Goodnight owns the whole box. A fresh rhythm will summon the heart
    // after waking; even a half-learned pre-sleep rhythm must not combine
    // with tomorrow's first presses.
    if (p.ticking) s_engine->noteOff(kPulseId);
    p.ticking = false;
    p.count = 0;
    p.beatsAlone = 0;
    p.beatsPlayed = 0;
    p.step = 0;
    return;
  }
  if (!p.ticking) return;
  if (now < p.nextBeatAt) return;
  if (now - p.lastOnset > p.period * 1.5) ++p.beatsAlone;
  const float fade = 1.0f - (float)p.beatsAlone / 6.0f;
  if (fade <= 0.0f) {  // the player drifted away — the heart lets go
    p.ticking = false;
    p.count = 0;
    p.step = 0;
    p.beatsPlayed = 0;
    return;
  }
  if (p.beatsPlayed < 12) ++p.beatsPlayed;
  const float warm =  // the heart fades IN too — no sudden metronome
      p.beatsPlayed < 3 ? 0.3f + (float)p.beatsPlayed * 0.23f : 1.0f;

  // What the heart plays: an arpeggio of YOUR background chord, one note
  // per beat, accent on the start of each cycle. Default 1/1+3/2 gives a
  // root-fifth heartbeat; a hand-picked 4-note chord becomes a 4-note riff.
  float cents = 0.0f, accent = 1.0f;
  const int bgN = ambient::backgroundNoteCount();
  if (bgN > 0) {
    const int i = p.step % bgN;
    cents = ambient::backgroundNoteCents(i);
    accent = i == 0 ? 1.2f : 0.8f;
    ++p.step;
  }
  float vel = 0.17f * fade * warm * accent;
  if (vel > 0.24f) vel = 0.24f;
  // MUSICBOX ends by itself — no note-off bookkeeping; the FIFO preset
  // sandwich is race-free (single producer, queue drained per render)
  s_engine->setParam(ga::Param::TimbrePreset, (float)kMusicboxPreset);
  s_engine->noteOn(kPulseId, cents, vel);
  s_engine->setParam(ga::Param::TimbrePreset, (float)settings::preset());
  p.nextBeatAt += p.period;
  while (p.nextBeatAt < now) p.nextBeatAt += p.period;  // never spiral
}

}  // namespace pulse
