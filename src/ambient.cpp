#include "ambient.h"

#include <Arduino.h>
#include <math.h>

#include "ga_dsp.h"
#include "gk_lullaby.h"
#include "hal/audio_out.h"

namespace {

ga::Engine* s_engine = nullptr;

// --- background (tampura) ---
struct BgNote { float cents; int32_t id; };
BgNote s_bg[4] = {{0.0f, 1000}, {702.0f, 1001}, {0.0f, -1}, {0.0f, -1}};
int s_bgCount = 2;
bool s_bgOn = true;
bool s_bgCustom = false;  // a hand-picked background silences the weather vote
gk::BackgroundId s_bgPreset = gk::BackgroundId::Drone;
uint32_t s_bgNextId = 2;  // rotates through ids 1000..1015

// --- weather ---
uint32_t s_weatherLastMs = 0;
int s_fifthVariant = 0;  // 0 = 3/2, 1 = 7/4 (default background only)
float s_cutoffBase = 7500.0f;
float s_spaceBase = 0.5f;  // przechył do/od siebie: głębia przestrzeni

gk::Garden s_garden;
uint32_t s_lastInputMs = 0;
uint32_t s_nextGhostMs = 0;
bool s_nextGhostPending = false;
uint32_t s_ghostMask = 0;
int s_ghostSlot = 0;
ambient::GardenPhrase s_ghostPhrase;
int s_ghostPhrasePos = 0;
uint32_t s_ghostPhraseNextMs = 0;
float s_ghostPhraseTranspose = 0.0f;
float s_ghostPhraseVel = 0.0f;
bool s_ghostPhraseActive = false;
// ghosts continue sessions, they never start them: no sowing until a real
// human act this session (a restored garden alone must stay silent)
bool s_played = false;
uint32_t s_greetAtMs = 0;
bool s_greetPending = false;
uint32_t s_autosaveAtMs = 0;
bool s_autosavePending = false;
uint64_t s_keysHeld = 0;
ambient::SaveRequest s_saveRequest = ambient::SaveRequest::None;
ambient::BeatGrid s_beatGrid;

// --- goodnight lullaby ---
gk::LullabySequencer s_lullaby;
constexpr int32_t kGhostIdBase = 1100;
constexpr int32_t kLullabyVoiceId = 1104;
// 7 s ciszy wystarczy — przy 12 s pamięć pudełka była praktycznie
// niesłyszalna w normalnej zabawie (dziecko nie robi tak długich pauz)
constexpr uint32_t kGhostIdleMs = 7000;
struct PendingOff { uint32_t atMs; int32_t id; };
PendingOff s_pending[6];
int s_pendingCount = 0;

int s_uiPreset = 3;           // what the player's keys currently use
constexpr int kBgPreset = 1;  // the background always speaks ORGAN

// okno dla wizualizacji
float s_breathe = 1.0f;
float s_lastGhostSourceCents = 0.0f;
float s_lastGhostPlayedCents = 0.0f;
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

float gardenRandom(void*) { return rnd01(); }

uint32_t replayGapMs(uint16_t recordedMs) {
  return gk::Garden::replayGapMs(recordedMs);
}

bool selectPhrase(ambient::GardenPhrase* out) {
  return s_garden.selectPhrase(out, gardenRandom, nullptr);
}

void scheduleNextGhost(uint32_t nowMs, bool firstAfterSilence) {
  uint32_t delay = firstAfterSilence ? 1500 + expDelayMs(6000.0f)
                                     : 800 + expDelayMs(9000.0f);
  uint32_t target = nowMs + delay;
  // The phrase starts on the remembered beat grid; its internal timing stays
  // exactly relative to the player instead of quantizing every note.
  if (s_beatGrid.periodSec > 0.15) {
    const double per = s_beatGrid.periodSec;
    const double nowSec = s_beatGrid.nowSec;
    const double at = nowSec + (double)delay * 0.001;
    double snapped =
        s_beatGrid.lastOnsetSec +
        round((at - s_beatGrid.lastOnsetSec) / per) * per;
    while (snapped < nowSec + 0.2) snapped += per;
    const double snappedDelayMs = (snapped - nowSec) * 1000.0;
    if (snappedDelayMs >= 0.0 && snappedDelayMs < 2147483647.0)
      target = nowMs + (uint32_t)snappedDelayMs;
  }
  s_nextGhostMs = target;
  s_nextGhostPending = true;
}

int32_t allocateBgId() {
  // Engine voices are keyed by id. Never let a rotating weather voice reuse
  // the id of a still-active root and later cut it with its own note-off.
  for (int attempt = 0; attempt < 16; ++attempt) {
    const int32_t id = 1000 + (int32_t)(s_bgNextId++ & 15u);
    bool used = false;
    for (int i = 0; i < s_bgCount; ++i)
      if (s_bg[i].id == id) used = true;
    if (!used) return id;
  }
  return 1000;  // at most four ids are active, so this is unreachable
}

void bgNoteOn(int32_t id, float cents, float vel) {
  s_engine->noteOnPersistentWithPreset(id, cents, vel, kBgPreset);
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
    // The tape remains a free three-second memory. Only its quieter second
    // head borrows the player's discovered pulse, and gives it back gently
    // when the PLL stops ticking.
    hal::echo().setRhythmicTap((float)s_beatGrid.periodSec,
                              s_beatGrid.ticking);
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
  if (!s_bgCustom && s_bgOn && s_bgCount >= 2 && !s_lullaby.active() &&
      gk::backgroundPreset(s_bgPreset).weatherMorph) {
    int want = s_fifthVariant;
    if (t2 > 0.6f) want = 1;
    else if (t2 < -0.6f) want = 0;
    if (want != s_fifthVariant) {
      s_fifthVariant = want;
      s_engine->noteOff(s_bg[1].id);
      s_bg[1].cents = want ? 968.8f : 702.0f;
      s_bg[1].id = allocateBgId();
      bgNoteOn(s_bg[1].id, s_bg[1].cents, want ? 0.14f : 0.16f);
    }
  }
}

void ghostSilenceAll() {
  s_ghostPhraseActive = false;
  s_nextGhostPending = false;
  s_ghostEvent = false;
  // Pending note-offs carry reusable ids. Clear them together with the notes
  // so an old timer can never cut a later phrase that reused the same slot.
  for (int i = 0; i < s_pendingCount; ++i) s_engine->noteOff(s_pending[i].id);
  s_pendingCount = 0;
  s_engine->noteOff(kLullabyVoiceId);
  if (!s_ghostMask) return;
  for (int i = 0; i < 4; ++i)
    if (s_ghostMask & (1u << i)) s_engine->noteOff(kGhostIdBase + i);
  s_ghostMask = 0;
}

// one ghost voice: per-event soft attack + scheduled note-off + viz event
void ghostPlay(float sourceCents, float playedCents, float vel, float attack,
               uint32_t nowMs, uint32_t holdMs) {
  const int slot = s_ghostSlot++ & 3;
  const int32_t id = kGhostIdBase + slot;
  s_ghostMask |= (1u << slot);
  // The attack belongs to this event. A full queue can no longer accept a
  // temporary global change and lose its restore, leaving later keys slow.
  s_engine->noteOnWithPreset(id, playedCents, vel, s_uiPreset, attack);
  s_lastGhostSourceCents = sourceCents;
  s_lastGhostPlayedCents = playedCents;
  s_ghostEvent = true;
  if (s_pendingCount < 6)
    s_pending[s_pendingCount++] = {nowMs + holdMs, id};
}

// The lullaby is one storyteller, not a growing chord. Reusing one id keeps
// its phrase contour coherent and cannot exhaust the ten-voice engine when
// recorded notes are close together.
void lullabyPlay(float cents, float vel) {
  // A bedtime voice must not inherit a bright/percussive player preset.
  // PURE plus a long attack leaves the remembered contour intact, but takes
  // the hard edge off CHIME/MUSICBOX and lets it disappear into the room.
  s_engine->noteOnWithPreset(kLullabyVoiceId, cents, vel, 0, 0.72f);
  s_lastGhostSourceCents = cents;
  s_lastGhostPlayedCents = cents;
  s_ghostEvent = true;
}

// scheduled ghost note-offs — drained ALWAYS (also during the lullaby and
// the sleep after it; starving these once left ghost chords humming all
// night through the "real silence")
void drainGhostOffs(uint32_t nowMs) {
  for (int i = 0; i < s_pendingCount;) {
    if ((int32_t)(nowMs - s_pending[i].atMs) >= 0) {
      s_engine->noteOff(s_pending[i].id);
      const int slot = (int)(s_pending[i].id - kGhostIdBase);
      if (slot >= 0 && slot < 4) s_ghostMask &= ~(1u << slot);
      s_pending[i] = s_pending[--s_pendingCount];
    } else {
      ++i;
    }
  }
}

void startGhostPhrase(uint32_t nowMs) {
  ambient::GardenPhrase phrase;
  if (!selectPhrase(&phrase)) return;
  ghostSilenceAll();
  s_ghostPhrase = phrase;
  s_ghostPhrasePos = 0;
  s_ghostPhraseNextMs = nowMs;
  s_ghostPhraseVel = 0.25f + 0.11f * rnd01();
  const float r = rnd01();
  // Phrase contour is law: if seasoned, every note moves by the same amount.
  if (r < 0.75f) s_ghostPhraseTranspose = 0.0f;
  else if (r < 0.84f) s_ghostPhraseTranspose = 1200.0f;
  else if (r < 0.92f) s_ghostPhraseTranspose = 702.0f;
  else s_ghostPhraseTranspose = -498.0f;
  s_ghostPhraseActive = true;
}

void ghostPhraseTick(uint32_t nowMs) {
  if (!s_ghostPhraseActive ||
      (int32_t)(nowMs - s_ghostPhraseNextMs) < 0) return;
  const int i = s_ghostPhrasePos;
  const float source = s_ghostPhrase.note[i].cents;
  const float played = source + s_ghostPhraseTranspose;
  const bool more = i + 1 < s_ghostPhrase.count;
  const uint32_t nextGap =
      more ? replayGapMs(s_ghostPhrase.note[i + 1].gapMs) : 0;
  const uint32_t hold =
      more ? (nextGap + 180 < 1000 ? nextGap + 180 : 1000) : 1400;
  ghostPlay(source, played, s_ghostPhraseVel * (i == 0 ? 1.0f : 0.88f),
            0.22f, nowMs, hold);
  ++s_ghostPhrasePos;
  if (more) {
    s_ghostPhraseNextMs = nowMs + nextGap;
  } else {
    s_ghostPhraseActive = false;
    scheduleNextGhost(nowMs, false);
  }
}

void gardenTick(uint32_t nowMs) {
  // the waking-up greeting: ONE remembered note, unless play began first
  if (s_greetPending && (int32_t)(nowMs - s_greetAtMs) >= 0) {
    s_greetPending = false;
    if (!s_played && s_garden.count() > 0) {
      const float freshest = s_garden.freshestCents();
      ghostPlay(freshest, freshest, 0.30f, 0.65f, nowMs, 2200);
    }
  }

  if (!s_played || nowMs - s_lastInputMs < kGhostIdleMs ||
      s_garden.count() == 0) {
    if (s_ghostPhraseActive) ghostSilenceAll();
    s_nextGhostPending = false;
    return;
  }
  if (s_ghostPhraseActive) {
    // At most one note per main-loop pass, so the single viz event cannot be
    // overwritten even for a recorded chord with a zero inter-onset gap.
    ghostPhraseTick(nowMs);
    return;
  }
  if (!s_nextGhostPending) {
    scheduleNextGhost(nowMs, true);
    return;
  }
  if ((int32_t)(nowMs - s_nextGhostMs) < 0) return;
  startGhostPhrase(nowMs);
  ghostPhraseTick(nowMs);  // the remembered gesture begins now
}

void lullabyTick(uint32_t nowMs) {
  const gk::LullabyEvent event = s_lullaby.tick(nowMs, s_garden);
  switch (event.type) {
    case gk::LullabyEventType::PlayNote:
      lullabyPlay(event.cents, event.velocity);
      // The world darkens and recedes as the remembered day winds down.
      s_cutoffBase = ga::clampf(4200.0f * exp2f(-2.8f * event.progress),
                               600.0f, 12000.0f);
      s_spaceBase =
          ga::clampf(0.50f + 0.18f * event.progress, 0.0f, 1.0f);
      break;
    case gk::LullabyEventType::EnterSleep:
      // The day is retold — real sleep. Pending note-offs are cleared with
      // their voices so no stale timer can leave a hum through the night.
      for (int i = 0; i < s_bgCount; ++i) s_engine->noteOff(s_bg[i].id);
      ghostSilenceAll();
      break;
    case gk::LullabyEventType::SaveDue:
      if (s_saveRequest == ambient::SaveRequest::None)
        s_saveRequest = ambient::SaveRequest::SoulAndSettings;
      break;
    case gk::LullabyEventType::None:
      break;
  }
}

}  // namespace

namespace ambient {

void init(ga::Engine* engine) {
  s_engine = engine;
  s_rng = micros() | 1u;
  s_lastInputMs = millis();
  backgroundApplyAll();
}

void tick(const BeatGrid& beatGrid) {
  if (!s_engine) return;
  s_beatGrid = beatGrid;
  const uint32_t now = millis();
  weatherTick(now);
  drainGhostOffs(now);  // in every state — ghosts must always ring out
  if (s_saveRequest != SaveRequest::None) return;
  if (s_autosavePending && s_keysHeld == 0 && !s_lullaby.active() &&
      (int32_t)(now - s_autosaveAtMs) >= 0) {
    // The soul is one 152-byte atomic blob now: one commit, five seconds
    // after the last captured note and before the first ghost at seven.
    s_saveRequest = SaveRequest::Soul;
    return;  // main saves before any ghost can begin in this pass
  }
  if (!s_lullaby.active()) gardenTick(now);  // the lullaby owns the night
  else lullabyTick(now);
}

SaveRequest saveRequest() { return s_saveRequest; }

void saveFinished(SaveRequest request, bool success) {
  if (request == SaveRequest::None || request != s_saveRequest) return;
  const uint32_t now = millis();
  if (request == SaveRequest::Soul) {
    if (success) {
      s_autosavePending = false;
    } else {
      s_autosaveAtMs = now + 30000;
      s_autosavePending = true;
    }
  } else {
    if (success) s_autosavePending = false;
    s_lullaby.acknowledgeSave(success, now);
  }
  s_saveRequest = SaveRequest::None;
}

bool lullabyStart() {
  // only after real play this session, only with something to sing
  if (!s_lullaby.start(millis(), s_played, s_garden)) return false;
  // Let the released daytime drone fade before the first remembered phrase;
  // otherwise its two tails stack under the opening and sound twice as big.
  // Do not let the first note arrive through the daytime-bright filter. The
  // rest of the retelling keeps darkening from this gentler starting point.
  s_cutoffBase = 4200.0f;
  s_spaceBase = 0.50f;
  // A face-down surface can keep a front key physically pressed. Release the
  // whole foreground now so no sustained grid voice survives into real sleep.
  s_engine->allNotesOff();
  ghostSilenceAll();
  return true;
}

void lullabyAbort() {
  if (!s_lullaby.active()) return;
  s_lullaby.abort();
  if (s_saveRequest == SaveRequest::SoulAndSettings)
    s_saveRequest = SaveRequest::None;
  ghostSilenceAll();
  s_cutoffBase = 7500.0f;  // the mode re-drives both from the next IMU step
  s_spaceBase = 0.5f;
  backgroundApplyAll();  // the tampura comes back with the morning
}

bool lullabyActive() { return s_lullaby.active(); }

void notePresence() {
  const uint32_t now = millis();
  s_lastInputMs = now;
  // A wind replay can last a little over six seconds. If a five-second soul
  // snapshot from the preceding key phrase is still pending, move it beyond
  // the whole gesture so flash parking never cuts a remembered sentence.
  if (s_autosavePending) s_autosaveAtMs = now + 7000;
  s_played = true;
  s_greetPending = false;  // the child is already here — no greeting needed
  if (s_lullaby.active()) lullabyAbort();  // a key always wakes the box
  ghostSilenceAll();
}

void keyState(int id, bool down) {
  if (id < 0 || id >= 56) return;
  const uint64_t mask = (uint64_t)1 << id;
  if (down) {
    s_keysHeld |= mask;
  } else {
    s_keysHeld &= ~mask;
    // A long sustain may outlive the original five-second deadline. Give its
    // release and echo a full quiet breath before touching flash.
    if (s_keysHeld == 0 && s_autosavePending)
      s_autosaveAtMs = millis() + 5000;
  }
}

void gardenPush(float cents) {
  const uint32_t now = millis();
  s_garden.push(cents, now);
  s_lastInputMs = now;
  s_autosaveAtMs = now + 5000;
  s_autosavePending = true;
  s_greetPending = false;
  // Every garden producer is a human act. Any played phrase can therefore
  // earn ghosts and the goodnight lullaby, just like on the host.
  s_played = true;
  ghostSilenceAll();
}

bool gardenPluck(GardenPhrase* phrase, float dir) {
  return s_garden.pluck(phrase, dir, gardenRandom, nullptr);
}

int gardenCount() { return s_garden.count(); }

int gardenPhraseCount() { return s_garden.phraseCount(); }

float gardenCents(int idxOldest) { return s_garden.cents(idxOldest); }

uint16_t gardenDelayMs(int idxOldest) {
  return s_garden.delayMs(idxOldest);
}

bool gardenStartsPhrase(int idxOldest) {
  return s_garden.startsPhrase(idxOldest);
}

void gardenRestore(const float* cents, const uint16_t* delayMs,
                   uint32_t phraseStartMask, int n) {
  s_garden.restore(cents, delayMs, phraseStartMask, n);
  // deliberately NOT s_played — a restored garden waits for a human act
}

void scheduleGreeting() {
  if (s_garden.count() > 0 && !s_played) {
    s_greetAtMs = millis() + 2500;
    s_greetPending = true;
  }
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
  const int32_t id = allocateBgId();
  s_bg[s_bgCount] = {cents, id};
  ++s_bgCount;
  if (s_bgOn) bgNoteOn(id, cents, s_bgCount == 1 ? 0.22f : 0.16f);
}

void backgroundSetPreset(gk::BackgroundId preset) {
  const gk::BackgroundPreset& selected = gk::backgroundPreset(preset);
  const bool removedCustomChord = s_bgCustom;
  for (int i = 0; i < s_bgCount; ++i) s_engine->noteOff(s_bg[i].id);

  s_bgCount = 0;
  s_bgCustom = false;
  s_bgPreset = selected.id;
  s_bgOn = selected.noteCount > 0;
  s_fifthVariant = 0;
  for (int i = 0; i < selected.noteCount; ++i) {
    const int32_t id = allocateBgId();
    s_bg[s_bgCount++] = {selected.cents[i], id};
  }
  for (int i = s_bgCount; i < 4; ++i) s_bg[i] = {0.0f, -1};
  backgroundApplyAll();
  // A custom chord lives in the Soul snapshot, whereas the built-in preset
  // lives in Settings. Clearing the former must eventually update both stores.
  if (removedCustomChord) {
    s_autosaveAtMs = millis() + 5000;
    s_autosavePending = true;
  }
}

gk::BackgroundId backgroundSelectedPreset() { return s_bgPreset; }

void backgroundRefresh() {
  // A mode switch sends all-notes-off. Do not let its cleanup resurrect the
  // tampura while the goodnight ritual owns the box; wake-up restores it.
  if (!s_lullaby.active()) backgroundApplyAll();
}
int backgroundCount() { return s_bgCount; }
bool backgroundIsCustom() { return s_bgCustom; }

void backgroundRestoreChord(const float* cents, int n) {
  if (n < 0 || n > 4 || (n > 0 && !cents)) return;
  for (int i = 0; i < s_bgCount; ++i) s_engine->noteOff(s_bg[i].id);
  s_bgCount = n;
  s_bgCustom = true;  // a remembered chord is law, like a hand-picked one
  for (int i = 0; i < n; ++i)
    s_bg[i] = {cents[i], allocateBgId()};
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

bool pollGhost(float* sourceCents, float* playedCents) {
  if (!s_ghostEvent) return false;
  s_ghostEvent = false;
  if (sourceCents) *sourceCents = s_lastGhostSourceCents;
  if (playedCents) *playedCents = s_lastGhostPlayedCents;
  return true;
}

void setPreset(int idx) {
  if (idx < 0) idx = 0;
  if (idx >= ga::kNumTimbrePresets) idx = ga::kNumTimbrePresets - 1;
  s_uiPreset = idx;
}

}  // namespace ambient
