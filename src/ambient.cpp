#include "ambient.h"

#include <Arduino.h>
#include <math.h>

#include "ga_dsp.h"
#include "gk_lullaby.h"
#include "gk_phrase_player.h"
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
gk::WorldId s_world = gk::WorldId::Meadow;
gk::WorldSound s_worldSound = gk::worldSound(gk::WorldId::Meadow);

gk::Garden s_garden;
uint32_t s_lastInputMs = 0;
uint32_t s_nextGhostMs = 0;
bool s_nextGhostPending = false;
gk::PhrasePlayer s_ghostPlayer;
float s_ghostPhraseTranspose = 0.0f;
float s_ghostPhraseVel = 0.0f;
bool s_greetingActive = false;
uint32_t s_greetingOffMs = 0;
// ghosts continue sessions, they never start them: no sowing until a real
// human act this session (a restored garden alone must stay silent)
bool s_played = false;
uint32_t s_greetAtMs = 0;
bool s_greetPending = false;
uint32_t s_autosaveAtMs = 0;
bool s_autosavePending = false;
bool s_settingsSavePending = false;
uint64_t s_keysHeld = 0;
bool s_memoryPlaying = false;
ambient::SaveRequest s_saveRequest = ambient::SaveRequest::None;
ambient::BeatGrid s_beatGrid;

// --- goodnight lullaby ---
gk::LullabySequencer s_lullaby;
constexpr int32_t kGhostIdBase = 1100;
constexpr int32_t kLullabyVoiceId = 1110;
// 7 s ciszy wystarczy — przy 12 s pamięć pudełka była praktycznie
// niesłyszalna w normalnej zabawie (dziecko nie robi tak długich pauz)
constexpr uint32_t kGhostIdleMs = 7000;
int s_uiPreset = 3;           // what the player's keys currently use
constexpr int kBgPreset = 1;  // the background always speaks ORGAN

// okno dla wizualizacji
float s_breathe = 1.0f;
struct GhostVisual { float source; float played; };
GhostVisual s_ghostEvents[gk::kGardenPhraseMax];
int s_ghostEventCount = 0;

void showGhost(float source, float played) {
  if (s_ghostEventCount < gk::kGardenPhraseMax)
    s_ghostEvents[s_ghostEventCount++] = {source, played};
}

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

  // Ease into the world's room and tape at UI rate; no extra sample-rate DSP.
  const auto& target = gk::worldSound(s_world);
  constexpr float follow = 0.08f;  // ~0.4 s at the weather's 30 Hz rate
  s_worldSound.echoLevelScale += follow * (target.echoLevelScale - s_worldSound.echoLevelScale);
  s_worldSound.echoFeedback += follow * (target.echoFeedback - s_worldSound.echoFeedback);
  s_worldSound.reverbLevelScale += follow * (target.reverbLevelScale - s_worldSound.reverbLevelScale);
  s_worldSound.room += follow * (target.room - s_worldSound.room);
  s_worldSound.damp += follow * (target.damp - s_worldSound.damp);
  hal::reverb().setRoom(s_worldSound.room);
  hal::reverb().setDamp(s_worldSound.damp);

  // głębia z przechyłu do/od siebie: mnożnik na echo i pogłos pogody
  const float space = 0.5f + s_spaceBase;  // 0.5..1.5, neutralnie 1.0
  if (hal::echoAvailable()) {
    // The tape remains a free three-second memory. Only its quieter second
    // head borrows the player's discovered pulse, and gives it back gently
    // when the PLL stops ticking.
    hal::echo().setRhythmicTap((float)s_beatGrid.periodSec,
                              s_beatGrid.ticking);
    hal::echo().setLevel(ga::clampf(
        (0.45f + 0.25f * breathe + 0.05f * t1) * space *
            s_worldSound.echoLevelScale, 0.04f, 0.90f));
    hal::echo().setFeedback(
        ga::clampf(s_worldSound.echoFeedback + 0.10f * breathe + 0.05f * t2,
                   0.10f, 0.68f));
  }
  hal::reverb().setWet(ga::clampf(
      (0.30f * (0.85f + 0.35f * breathe) + 0.03f * t3) * space *
          s_worldSound.reverbLevelScale, 0.03f, 0.70f));
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
  const auto batch = s_ghostPlayer.cancel();
  for (int i = 0; i < batch.count; ++i)
    s_engine->noteOff(kGhostIdBase + batch.events[i].slot);
  if (s_greetingActive) s_engine->noteOff(kGhostIdBase);
  s_greetingActive = false;
  s_nextGhostPending = false;
  s_ghostEventCount = 0;
  s_engine->noteOff(kLullabyVoiceId);
}

void greetingPlay(float cents, uint32_t nowMs) {
  s_engine->noteOnWithPreset(kGhostIdBase, cents, 0.30f, s_uiPreset, 0.65f);
  showGhost(cents, cents);
  s_greetingActive = true;
  s_greetingOffMs = nowMs + 2200;
}

// The lullaby is one storyteller, not a growing chord. Reusing one id keeps
// its phrase contour coherent and cannot exhaust the ten-voice engine when
// recorded notes are close together.
void lullabyPlay(float cents, float vel) {
  // A bedtime voice must not inherit a bright/percussive player preset.
  // PURE plus a long attack leaves the remembered contour intact, but takes
  // the hard edge off CHIME/MUSICBOX and lets it disappear into the room.
  s_engine->noteOnWithPreset(kLullabyVoiceId, cents, vel, 0, 0.72f);
  showGhost(cents, cents);
}

void drainGhostOffs(uint32_t nowMs) {
  if (s_greetingActive && (int32_t)(nowMs - s_greetingOffMs) >= 0) {
    s_engine->noteOff(kGhostIdBase);
    s_greetingActive = false;
  }
}

void startGhostPhrase(uint32_t nowMs) {
  ambient::GardenPhrase phrase;
  if (!selectPhrase(&phrase)) return;
  ghostSilenceAll();
  s_ghostPlayer.start(phrase, nowMs);
  s_ghostPhraseVel = 0.25f + 0.11f * rnd01();
  const float r = rnd01();
  // Phrase contour is law: if seasoned, every note moves by the same amount.
  if (r < 0.75f) s_ghostPhraseTranspose = 0.0f;
  else if (r < 0.84f) s_ghostPhraseTranspose = 1200.0f;
  else if (r < 0.92f) s_ghostPhraseTranspose = 702.0f;
  else s_ghostPhraseTranspose = -498.0f;
}

void ghostPhraseTick(uint32_t nowMs) {
  const auto batch = s_ghostPlayer.tick(nowMs);
  for (int i = 0; i < batch.count; ++i) {
    const auto& event = batch.events[i];
    const int32_t id = kGhostIdBase + event.slot;
    if (!event.down) {
      s_engine->noteOff(id);
      continue;
    }
    const float played = event.note.cents + s_ghostPhraseTranspose;
    const float vel = s_ghostPhraseVel * event.note.velocity / 90.0f;
    const float attack = fminf(0.22f, event.note.holdMs * 0.00025f);
    s_engine->noteOnWithPreset(id, played, vel, s_uiPreset, attack);
    showGhost(event.note.cents, played);
  }
  if (!s_ghostPlayer.active()) scheduleNextGhost(nowMs, false);
}

void gardenTick(uint32_t nowMs) {
  // the waking-up greeting: ONE remembered note, unless play began first
  if (s_greetPending && (int32_t)(nowMs - s_greetAtMs) >= 0) {
    s_greetPending = false;
    if (!s_played && s_garden.count() > 0) {
      const float freshest = s_garden.freshestCents();
      greetingPlay(freshest, nowMs);
    }
  }

  if (!s_played || nowMs - s_lastInputMs < kGhostIdleMs ||
      s_garden.count() == 0) {
    if (s_ghostPlayer.active()) ghostSilenceAll();
    s_nextGhostPending = false;
    return;
  }
  if (s_ghostPlayer.active()) {
    // Chord notes share a batch; the visual queue preserves every seed.
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
  if (s_autosavePending && s_keysHeld == 0 && !s_memoryPlaying &&
      !s_ghostPlayer.active() && !s_lullaby.active() &&
      (int32_t)(now - s_autosaveAtMs) >= 0) {
    // The soul is one atomic blob: one commit, five seconds
    // after the last captured note and before the first ghost at seven.
    s_saveRequest = s_settingsSavePending ? SaveRequest::SoulAndSettings : SaveRequest::Soul;
    return;  // main saves before any ghost can begin in this pass
  }
  if (!s_lullaby.active()) {
    if (!s_memoryPlaying && (s_keysHeld == 0 || s_ghostPlayer.active()))
      gardenTick(now);
  } else {
    lullabyTick(now);
  }
}

SaveRequest saveRequest() { return s_saveRequest; }

void saveFinished(SaveRequest request, bool success) {
  if (request == SaveRequest::None || request != s_saveRequest) return;
  const uint32_t now = millis();
  if (request == SaveRequest::Soul || !s_lullaby.active()) {
    if (success) {
      s_autosavePending = false;
      s_settingsSavePending = false;
    } else {
      s_autosaveAtMs = now + 30000;
      s_autosavePending = true;
    }
  } else {
    if (success) {
      s_autosavePending = false;
      s_settingsSavePending = false;
    }
    s_lullaby.acknowledgeSave(success, now);
  }
  s_saveRequest = SaveRequest::None;
}

void settingsChanged() {
  const uint32_t now = millis();
  s_settingsSavePending = true;
  s_autosavePending = true;
  s_autosaveAtMs = now + 5000;
  s_lastInputMs = now;
  s_greetPending = false;
  if (s_engine) ghostSilenceAll();
}

void setWorld(gk::WorldId world) { s_world = world; }

bool lullabyStart() {
  // only after real play this session, only with something to sing
  if (!s_lullaby.start(millis(), s_played, s_garden)) return false;
  s_garden.releaseAll(millis());
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
  // Defer pending saves on presence. memoryPlaying additionally protects the
  // whole recall, including long held notes, until its final release.
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

bool keysHeld() { return s_keysHeld != 0; }

void memoryPlaying(bool playing) {
  if (s_memoryPlaying == playing) return;
  s_memoryPlaying = playing;
  if (!playing) {
    s_lastInputMs = millis();
    if (s_autosavePending) s_autosaveAtMs = millis() + 5000;
  }
}

void gardenReleaseAll() { s_garden.releaseAll(millis()); }

void gardenRelease(int32_t id) { s_garden.noteOff(id, millis()); }

void gardenPush(int32_t id, float cents) {
  const uint32_t now = millis();
  s_garden.noteOn(id, cents, now);
  s_lastInputMs = now;
  s_autosaveAtMs = now + 5000;
  s_autosavePending = true;
  s_greetPending = false;
  // Every garden producer is a human act. Any played phrase can therefore
  // earn ghosts and the goodnight lullaby, just like on the host.
  s_played = true;
  ghostSilenceAll();
}

bool gardenRecall(GardenPhrase* phrase) {
  // Recalling ends the teaching gesture: the next key starts a new phrase,
  // even if a short recall finished before the normal silence boundary.
  s_garden.releaseAll(millis());
  return s_garden.latestPhrase(phrase);
}

int gardenCount() { return s_garden.count(); }

int gardenPhraseCount() { return s_garden.phraseCount(); }

float gardenCents(int idxOldest) { return s_garden.cents(idxOldest); }

uint16_t gardenDelayMs(int idxOldest) {
  return s_garden.delayMs(idxOldest);
}

uint16_t gardenHoldMs(int idxOldest) { return s_garden.holdMs(idxOldest); }
uint8_t gardenVelocity(int idxOldest) { return s_garden.velocity(idxOldest); }

bool gardenStartsPhrase(int idxOldest) {
  return s_garden.startsPhrase(idxOldest);
}

void gardenRestore(const float* cents, const uint16_t* delayMs,
                   uint32_t phraseStartMask, int n,
                   const uint16_t* holdMs, const uint8_t* velocity) {
  s_garden.restore(cents, delayMs, phraseStartMask, n, holdMs, velocity);
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
  if (s_ghostEventCount == 0) return false;
  const GhostVisual event = s_ghostEvents[0];
  for (int i = 1; i < s_ghostEventCount; ++i)
    s_ghostEvents[i - 1] = s_ghostEvents[i];
  --s_ghostEventCount;
  if (sourceCents) *sourceCents = event.source;
  if (playedCents) *playedCents = event.played;
  return true;
}

void setPreset(int idx) {
  if (idx < 0) idx = 0;
  if (idx >= ga::kNumTimbrePresets) idx = ga::kNumTimbrePresets - 1;
  s_uiPreset = idx;
}

}  // namespace ambient
