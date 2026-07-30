#include "ambient.h"

#include <Arduino.h>
#include <math.h>

#include "ga_dsp.h"
#include "hal/audio_out.h"
#include "pulse.h"
#include "soul.h"

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
float s_cutoffBase = 7500.0f;
float s_spaceBase = 0.5f;  // przechył do/od siebie: głębia przestrzeni

// --- ghost garden ---
struct Mem { float cents; };
constexpr int kGardenCap = 32;
Mem s_ring[kGardenCap];
int s_head = 0, s_count = 0;
uint32_t s_lastInputMs = 0;
uint32_t s_nextGhostMs = 0;
uint32_t s_ghostMask = 0;
int s_ghostSlot = 0;
// ghosts continue sessions, they never start them: no sowing until a real
// human act this session (a restored garden alone must stay silent)
bool s_played = false;
uint32_t s_greetAtMs = 0;  // the one waking-up note (soul), 0 = none

// --- goodnight lullaby ---
enum class Lull : uint8_t { None, Singing, Sleeping };
Lull s_lull = Lull::None;
uint32_t s_lullNextMs = 0;
uint32_t s_lullSaveAtMs = 0;  // soul save, scheduled INTO the silence —
                              // an NVS write stalls core 0 (flash cache)
                              // and would scratch audibly mid-music
float s_lullGapSec = 0.0f;
float s_lullVel = 0.0f;
int s_lullIdx = 0;  // walks the garden oldest -> newest: the day, retold
constexpr int32_t kGhostIdBase = 1100;
// 7 s ciszy wystarczy — przy 12 s pamięć pudełka była praktycznie
// niesłyszalna w normalnej zabawie (dziecko nie robi tak długich pauz)
constexpr uint32_t kGhostIdleMs = 7000;
float s_presetAttack = 0.003f;
struct PendingOff { uint32_t atMs; int32_t id; };
PendingOff s_pending[6];
int s_pendingCount = 0;

int s_uiPreset = 3;           // what the player's keys currently use
constexpr int kBgPreset = 1;  // the background always speaks DRONE

// okno dla wizualizacji
float s_breathe = 1.0f;
float s_lastGhostCents = 0.0f;
bool s_ghostEvent = false;

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

void bgNoteOn(int32_t id, float cents, float vel) {
  // timbre sandwich: FIFO queue, single producer — race-free
  s_engine->setParam(ga::Param::TimbrePreset, (float)kBgPreset);
  s_engine->noteOn(id, cents, vel);
  s_engine->setParam(ga::Param::TimbrePreset, (float)s_uiPreset);
}

void backgroundApplyAll() {
  for (int i = 0; i < s_bgCount; ++i) {
    if (s_bgOn)
      bgNoteOn(s_bg[i].id, s_bg[i].cents, i == 0 ? 0.22f : 0.16f);
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
  s_breathe = breathe;

  // głębia z przechyłu do/od siebie: mnożnik na echo i pogłos pogody
  const float space = 0.5f + s_spaceBase;  // 0.5..1.5, neutralnie 1.0
  if (hal::echoAvailable()) {
    hal::echo().setLevel(ga::clampf(
        (0.45f + 0.25f * breathe + 0.05f * t1) * space, 0.15f, 0.90f));
    hal::echo().setFeedback(
        ga::clampf(0.52f + 0.10f * breathe + 0.05f * t2, 0.40f, 0.68f));
  }
  hal::reverb().setWet(ga::clampf(
      (0.30f * (0.85f + 0.35f * breathe) + 0.03f * t3) * space, 0.06f, 0.70f));
  const float ratio = (0.95f + 0.15f * t3) * (1.0f - 0.07f * breathe);
  s_engine->setParam(ga::Param::FilterCutoffHz,
                     ga::clampf(s_cutoffBase * ratio, 300.0f, 12000.0f));

  // background voting (default set only — a hand-picked chord is law;
  // asleep, the weather must not resurrect the tampura)
  if (!s_bgCustom && s_bgOn && s_bgCount >= 2 && s_lull == Lull::None) {
    int want = s_fifthVariant;
    if (t2 > 0.6f) want = 1;
    else if (t2 < -0.6f) want = 0;
    if (want != s_fifthVariant) {
      s_fifthVariant = want;
      s_engine->noteOff(s_bg[1].id);
      s_bg[1].cents = want ? 968.8f : 702.0f;
      s_bg[1].id = 1000 + (s_bgNextId++ & 15);
      bgNoteOn(s_bg[1].id, s_bg[1].cents, want ? 0.14f : 0.16f);
    }
  }
}

void ghostSilenceAll() {
  if (!s_ghostMask) return;
  for (int i = 0; i < 4; ++i)
    if (s_ghostMask & (1u << i)) s_engine->noteOff(kGhostIdBase + i);
  s_ghostMask = 0;
}

// one ghost voice: soft attack sandwich + scheduled note-off + viz event
void ghostPlay(float cents, float vel, float attack, uint32_t nowMs) {
  const int slot = s_ghostSlot++ & 3;
  const int32_t id = kGhostIdBase + slot;
  s_ghostMask |= (1u << slot);
  // soft attack just for this note — the engine queue is FIFO, single
  // producer, drained fully before each render: the sandwich is race-free
  s_engine->setParam(ga::Param::EnvAttack, attack);
  s_engine->noteOn(id, cents, vel);
  s_engine->setParam(ga::Param::EnvAttack, s_presetAttack);
  s_lastGhostCents = cents;  // wizualizacja rozbłyśnie odpowiednią iskierkę
  s_ghostEvent = true;
  if (s_pendingCount < 6)
    s_pending[s_pendingCount++] = {
        nowMs + 2000 + (uint32_t)(2500.0f * rnd01()), id};
}

// scheduled ghost note-offs — drained ALWAYS (also during the lullaby and
// the sleep after it; starving these once left ghost chords humming all
// night through the "real silence")
void drainGhostOffs(uint32_t nowMs) {
  for (int i = 0; i < s_pendingCount;) {
    if ((int32_t)(nowMs - s_pending[i].atMs) >= 0) {
      s_engine->noteOff(s_pending[i].id);
      s_pending[i] = s_pending[--s_pendingCount];
    } else {
      ++i;
    }
  }
}

void gardenTick(uint32_t nowMs) {
  // the waking-up greeting: ONE remembered note, unless play began first
  if (s_greetAtMs && (int32_t)(nowMs - s_greetAtMs) >= 0) {
    s_greetAtMs = 0;
    if (!s_played && s_count > 0) {
      const int idx = (s_head - 1 + kGardenCap) % kGardenCap;  // freshest
      ghostPlay(s_ring[idx].cents, 0.30f, 1.6f, nowMs);
    }
  }

  if (!s_played || nowMs - s_lastInputMs < kGhostIdleMs || s_count == 0) {
    s_nextGhostMs = 0;
    return;
  }
  if (s_nextGhostMs == 0) {
    s_nextGhostMs = nowMs + 1500 + expDelayMs(6000.0f);
    return;
  }
  if ((int32_t)(nowMs - s_nextGhostMs) < 0) return;
  s_nextGhostMs = nowMs + expDelayMs(9000.0f);
  // the ghosts remember your last pulse: sowing snaps to that beat grid
  if (pulse::memoryPeriodSec() > 0.15) {
    const double per = pulse::memoryPeriodSec();
    const double at = (double)s_nextGhostMs * 0.001;
    double snapped =
        pulse::lastOnsetSec() + round((at - pulse::lastOnsetSec()) / per) * per;
    while (snapped * 1000.0 < (double)nowMs + 200.0) snapped += per;
    s_nextGhostMs = (uint32_t)(snapped * 1000.0);
  }

  const float u = rnd01();
  const int back = (int)(u * u * (float)(s_count - 1));
  const int idx = (s_head - 1 - back + 2 * kGardenCap) % kGardenCap;
  float cents = s_ring[idx].cents;
  const float r = rnd01();
  // transpositions balanced both ways — with up-only sprinkles a quarter
  // hour of play drifted the memories into squeaks
  if (r < 0.40f) { /* as played */ }
  else if (r < 0.65f) cents += 1200.0f;
  else if (r < 0.80f) cents += 702.0f;
  else cents -= 498.0f;

  ghostPlay(cents, 0.26f + 0.16f * rnd01(), 1.2f, nowMs);
}

void lullabyTick(uint32_t nowMs) {
  if (s_lull == Lull::Sleeping) {
    // the soul saves itself INTO the silence, once everything rang out
    if (s_lullSaveAtMs && (int32_t)(nowMs - s_lullSaveAtMs) >= 0) {
      s_lullSaveAtMs = 0;
      soul::save();
    }
    return;
  }
  if (s_lull != Lull::Singing) return;
  if ((int32_t)(nowMs - s_lullNextMs) < 0) return;
  if (s_lullIdx >= s_count) {
    // the day is retold — REAL sleep: the tampura bows out, the last
    // lullaby ghosts are silenced too (their scheduled note-offs would
    // otherwise never fire and the "silence" hummed a chord all night)
    s_lull = Lull::Sleeping;
    for (int i = 0; i < s_bgCount; ++i) s_engine->noteOff(s_bg[i].id);
    ghostSilenceAll();
    s_lullSaveAtMs = nowMs + 3000;  // save the day once the tails fade
    return;
  }
  const int idx = (s_head - s_count + s_lullIdx + 2 * kGardenCap) % kGardenCap;
  ghostPlay(s_ring[idx].cents, s_lullVel, 1.6f, nowMs);
  ++s_lullIdx;
  s_lullNextMs = nowMs + (uint32_t)(s_lullGapSec * 1000.0f);
  s_lullGapSec *= 1.09f;             // each note a little later...
  s_lullVel = fmaxf(0.10f, s_lullVel * 0.94f);  // ...and a little softer
  // and the world darkens and recedes with it
  const float prog = (float)s_lullIdx / (float)(s_count > 1 ? s_count : 1);
  s_cutoffBase = ga::clampf(7500.0f * exp2f(-3.5f * prog), 600.0f, 12000.0f);
  s_spaceBase = ga::clampf(0.5f + 0.4f * prog, 0.0f, 1.0f);
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
  drainGhostOffs(now);  // in every state — ghosts must always ring out
  if (s_lull == Lull::None) gardenTick(now);  // the lullaby owns the night
  else lullabyTick(now);
}

void lullabyStart() {
  // only after real play this session, only with something to sing
  if (s_lull != Lull::None || !s_played || s_count == 0) return;
  s_lull = Lull::Singing;
  s_lullIdx = 0;
  s_lullGapSec = 1.2f;
  s_lullVel = 0.30f;
  s_lullNextMs = millis() + 600;
  ghostSilenceAll();
}

void lullabyAbort() {
  if (s_lull == Lull::None) return;
  s_lull = Lull::None;
  s_lullSaveAtMs = 0;  // aborted before sleep: next settings visit saves
  ghostSilenceAll();
  s_cutoffBase = 7500.0f;  // the mode re-drives both from the next IMU step
  s_spaceBase = 0.5f;
  backgroundApplyAll();  // the tampura comes back with the morning
}

bool lullabyActive() { return s_lull != Lull::None; }

void notePresence() {
  s_lastInputMs = millis();
  s_played = true;
  s_greetAtMs = 0;  // the child is already here — no greeting needed
  if (s_lull != Lull::None) lullabyAbort();  // a key always wakes the box
  ghostSilenceAll();
}

void gardenPush(float cents) {
  s_ring[s_head] = {cents};
  s_head = (s_head + 1) % kGardenCap;
  if (s_count < kGardenCap) ++s_count;
  // every garden producer is a human act — keys, chimes AND singing (the
  // duet pushes sung notes here); a sung-only session must still earn
  // ghosts and the goodnight lullaby, like on the host
  s_played = true;
}

bool gardenPluck(float* cents, float dir) {
  if (s_count == 0) return false;
  const float u = rnd01();
  const int back = (int)(u * u * (float)(s_count - 1));  // świeże częściej
  const int idx = (s_head - 1 - back + 2 * kGardenCap) % kGardenCap;
  float c = s_ring[idx].cents;
  const float r = rnd01();
  // replay-first stays law (hardware verdict: transpositions blurred the
  // melody) — but the seasoning follows the hand: waving one way sprinkles
  // upward, the other way downward. The wind finally steers the memories.
  if (r < 0.78f) { /* jak zagrano */ }
  else if (dir >= 0.0f) c += r < 0.92f ? 1200.0f : 702.0f;
  else                  c -= r < 0.92f ? 498.0f : 1200.0f;
  if (cents) *cents = c;
  return true;
}

int gardenCount() { return s_count; }

float gardenCents(int idxOldest) {
  if (idxOldest < 0 || idxOldest >= s_count) return 0.0f;
  const int idx = (s_head - s_count + idxOldest + 2 * kGardenCap) % kGardenCap;
  return s_ring[idx].cents;
}

void gardenRestore(const float* cents, int n) {
  if (n > kGardenCap) n = kGardenCap;
  for (int i = 0; i < n; ++i) s_ring[i] = {cents[i]};
  s_head = n % kGardenCap;
  s_count = n;
  // deliberately NOT s_played — a restored garden waits for a human act
}

void scheduleGreeting() {
  if (s_count > 0 && !s_played) s_greetAtMs = millis() + 2500;
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
  if (s_bgOn) bgNoteOn(id, cents, s_bgCount == 1 ? 0.22f : 0.16f);
}

void backgroundSetEnabled(bool on) {
  s_bgOn = on;
  backgroundApplyAll();
}

void backgroundRefresh() { backgroundApplyAll(); }
bool backgroundEnabled() { return s_bgOn; }
int backgroundCount() { return s_bgCount; }
bool backgroundIsCustom() { return s_bgCustom; }

void backgroundRestoreChord(const float* cents, int n) {
  if (n < 1 || n > 4) return;
  for (int i = 0; i < s_bgCount; ++i) s_engine->noteOff(s_bg[i].id);
  s_bgCount = n;
  s_bgCustom = true;  // a remembered chord is law, like a hand-picked one
  for (int i = 0; i < n; ++i)
    s_bg[i] = {cents[i], 1000 + (s_bgNextId++ & 15)};
  backgroundApplyAll();
}

void setCutoffBase(float hz) { s_cutoffBase = ga::clampf(hz, 300.0f, 12000.0f); }

void setSpaceBase(float space01) { s_spaceBase = ga::clampf(space01, 0.0f, 1.0f); }

float breathe01() { return s_breathe; }

int backgroundNoteCount() { return s_bgOn ? s_bgCount : 0; }

float backgroundNoteCents(int i) {
  if (i < 0 || i >= s_bgCount) return 0.0f;
  return s_bg[i].cents;
}

bool pollGhost(float* cents) {
  if (!s_ghostEvent) return false;
  s_ghostEvent = false;
  if (cents) *cents = s_lastGhostCents;
  return true;
}

void setPreset(int idx) {
  if (idx < 0) idx = 0;
  if (idx >= ga::kNumTimbrePresets) idx = ga::kNumTimbrePresets - 1;
  s_uiPreset = idx;
  s_presetAttack = ga::timbrePreset(idx).attack;
}

}  // namespace ambient
