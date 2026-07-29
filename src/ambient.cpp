#include "ambient.h"

#include <Arduino.h>
#include <math.h>

#include "ga_dsp.h"
#include "hal/audio_out.h"

namespace {

ga::Engine* s_engine = nullptr;

// --- background (tampura) ---
struct BgNote { float cents; int32_t id; };
BgNote s_bg[4] = {{0.0f, 1000}, {702.0f, 1001}, {0.0f, -1}, {0.0f, -1}};
int s_bgCount = 2;
bool s_bgOn = true;
bool s_bgCustom = false;  // a hand-picked background silences the weather vote
int32_t s_bgNextId = 2;   // rotates through ids 1000..1015

// --- weather ---
uint32_t s_weatherLastMs = 0;
int s_fifthVariant = 0;  // 0 = 3/2, 1 = 7/4 (default background only)
float s_cutoffBase = 6000.0f;

// --- ghost garden ---
struct Mem { float cents; };
constexpr int kGardenCap = 32;
Mem s_ring[kGardenCap];
int s_head = 0, s_count = 0;
uint32_t s_lastInputMs = 0;
uint32_t s_nextGhostMs = 0;
uint32_t s_ghostMask = 0;
int s_ghostSlot = 0;
constexpr int32_t kGhostIdBase = 1100;
constexpr uint32_t kGhostIdleMs = 12000;
float s_presetAttack = 0.003f;
struct PendingOff { uint32_t atMs; int32_t id; };
PendingOff s_pending[6];
int s_pendingCount = 0;

uint32_t s_rng = 1;
float rnd01() {
  s_rng ^= s_rng << 13;
  s_rng ^= s_rng >> 17;
  s_rng ^= s_rng << 5;
  return (float)(s_rng >> 8) * (1.0f / 16777216.0f);
}

uint32_t expDelayMs(float meanMs) {
  return (uint32_t)(-meanMs * logf(1.0f - 0.9999f * rnd01()));
}

void backgroundApplyAll() {
  for (int i = 0; i < s_bgCount; ++i) {
    if (s_bgOn)
      s_engine->noteOn(s_bg[i].id, s_bg[i].cents, i == 0 ? 0.22f : 0.16f);
    else
      s_engine->noteOff(s_bg[i].id);
  }
}

void weatherTick(uint32_t nowMs) {
  if (nowMs - s_weatherLastMs < 33) return;
  s_weatherLastMs = nowMs;
  const float t = (float)nowMs * 0.001f;
  const float t1 = ga::sinTurns(t / 90.0f);
  const float t2 = ga::sinTurns(t / 210.0f);
  const float t3 = ga::sinTurns(t / 330.0f);
  const float env = hal::audioEnv();
  const float breathe = ga::clampf(1.0f - (env - 0.04f) / 0.18f, 0.0f, 1.0f);

  if (hal::echoAvailable()) {
    hal::echo().setLevel(
        ga::clampf(0.45f + 0.25f * breathe + 0.05f * t1, 0.30f, 0.80f));
    hal::echo().setFeedback(
        ga::clampf(0.52f + 0.10f * breathe + 0.05f * t2, 0.40f, 0.68f));
  }
  hal::reverb().setWet(
      ga::clampf(0.30f * (0.85f + 0.35f * breathe) + 0.03f * t3, 0.10f, 0.55f));
  const float ratio = (0.90f + 0.18f * t3) * (1.0f - 0.15f * breathe);
  s_engine->setParam(ga::Param::FilterCutoffHz,
                     ga::clampf(s_cutoffBase * ratio, 300.0f, 12000.0f));

  // background voting (default set only — a hand-picked chord is law)
  if (!s_bgCustom && s_bgOn && s_bgCount >= 2) {
    int want = s_fifthVariant;
    if (t2 > 0.6f) want = 1;
    else if (t2 < -0.6f) want = 0;
    if (want != s_fifthVariant) {
      s_fifthVariant = want;
      s_engine->noteOff(s_bg[1].id);
      s_bg[1].cents = want ? 968.8f : 702.0f;
      s_bg[1].id = 1000 + (s_bgNextId++ & 15);
      s_engine->noteOn(s_bg[1].id, s_bg[1].cents, want ? 0.14f : 0.16f);
    }
  }
}

void ghostSilenceAll() {
  if (!s_ghostMask) return;
  for (int i = 0; i < 4; ++i)
    if (s_ghostMask & (1u << i)) s_engine->noteOff(kGhostIdBase + i);
  s_ghostMask = 0;
}

void gardenTick(uint32_t nowMs) {
  // scheduled ghost note-offs
  for (int i = 0; i < s_pendingCount;) {
    if ((int32_t)(nowMs - s_pending[i].atMs) >= 0) {
      s_engine->noteOff(s_pending[i].id);
      s_pending[i] = s_pending[--s_pendingCount];
    } else {
      ++i;
    }
  }

  if (nowMs - s_lastInputMs < kGhostIdleMs || s_count == 0) {
    s_nextGhostMs = 0;
    return;
  }
  if (s_nextGhostMs == 0) {
    s_nextGhostMs = nowMs + 2000 + expDelayMs(9000.0f);
    return;
  }
  if ((int32_t)(nowMs - s_nextGhostMs) < 0) return;
  s_nextGhostMs = nowMs + expDelayMs(12000.0f);

  const float u = rnd01();
  const int back = (int)(u * u * (float)(s_count - 1));
  const int idx = (s_head - 1 - back + 2 * kGardenCap) % kGardenCap;
  float cents = s_ring[idx].cents;
  const float r = rnd01();
  if (r < 0.40f) { /* as played */ }
  else if (r < 0.70f) cents += 1200.0f;
  else if (r < 0.85f) cents += 702.0f;
  else cents -= 498.0f;

  const int slot = s_ghostSlot++ & 3;
  const int32_t id = kGhostIdBase + slot;
  s_ghostMask |= (1u << slot);
  // soft attack just for this note — the engine queue is FIFO, single
  // producer, drained fully before each render: the sandwich is race-free
  s_engine->setParam(ga::Param::EnvAttack, 1.2f);
  s_engine->noteOn(id, cents, 0.20f + 0.15f * rnd01());
  s_engine->setParam(ga::Param::EnvAttack, s_presetAttack);
  if (s_pendingCount < 6)
    s_pending[s_pendingCount++] = {
        nowMs + 2000 + (uint32_t)(2500.0f * rnd01()), id};
}

}  // namespace

namespace ambient {

void init(ga::Engine* engine) {
  s_engine = engine;
  s_rng = micros() | 1u;
  s_lastInputMs = millis();
  backgroundApplyAll();
}

void tick() {
  if (!s_engine) return;
  const uint32_t now = millis();
  weatherTick(now);
  gardenTick(now);
}

void notePresence() {
  s_lastInputMs = millis();
  ghostSilenceAll();
}

void gardenPush(float cents) {
  s_ring[s_head] = {cents};
  s_head = (s_head + 1) % kGardenCap;
  if (s_count < kGardenCap) ++s_count;
}

void backgroundToggleNote(float cents) {
  s_bgCustom = true;
  for (int i = 0; i < s_bgCount; ++i) {
    if (fabsf(s_bg[i].cents - cents) < 1.0f) {
      s_engine->noteOff(s_bg[i].id);
      for (int j = i; j < s_bgCount - 1; ++j) s_bg[j] = s_bg[j + 1];
      --s_bgCount;
      return;
    }
  }
  if (s_bgCount == 4) {  // full: the oldest bows out
    s_engine->noteOff(s_bg[0].id);
    for (int j = 0; j < 3; ++j) s_bg[j] = s_bg[j + 1];
    --s_bgCount;
  }
  const int32_t id = 1000 + (s_bgNextId++ & 15);
  s_bg[s_bgCount] = {cents, id};
  ++s_bgCount;
  if (s_bgOn) s_engine->noteOn(id, cents, s_bgCount == 1 ? 0.22f : 0.16f);
}

void backgroundSetEnabled(bool on) {
  s_bgOn = on;
  backgroundApplyAll();
}
bool backgroundEnabled() { return s_bgOn; }
int backgroundCount() { return s_bgCount; }

void setCutoffBase(float hz) { s_cutoffBase = ga::clampf(hz, 300.0f, 12000.0f); }
void setPresetAttack(float sec) { s_presetAttack = sec; }

}  // namespace ambient
