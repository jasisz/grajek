#include "mode_instrument.h"

#include <M5Unified.h>
#include <math.h>

#include "../ambient.h"
#include "../firefly.h"
#include "../goodnight.h"
#include "../i18n.h"
#include "../pulse.h"
#include "../settings.h"
#include "../viz.h"
#include "ga_dsp.h"
#include "ga_scales.h"
#include "gk_phrase_player.h"

using namespace ga;

namespace {
constexpr float kImuPeriod = 0.02f;  // odczyt IMU 50 Hz
constexpr float kNeutralCutoff = 7500.0f;

// machanie: histereza energii ruchu (po odjęciu grawitacji), w g
constexpr float kSwingOn = 0.55f;   // taki zamach gra
constexpr float kSwingOff = 0.30f;  // poniżej — uzbrajamy następny
constexpr uint32_t kSwingCooldownMs = 90;

// przechył boczny (jasność): czulszy na prośbę testera
constexpr float kTiltDead = 6.0f;
constexpr float kTiltFull = 32.0f;
// przechył do/od siebie (przestrzeń): większa martwa strefa, bo w rękach
// pudełko rzadko leży idealnie płasko
constexpr float kTiltFbDead = 10.0f;
constexpr float kTiltFbFull = 45.0f;

// id nut dzwonków — poza siatką (0..55), tłem (1000+) i duchami (1100+)
constexpr int32_t kChimeIdBase = 900;

// Gesture state lives at FILE scope so visits to settings do not reset the
// gravity filter and tilts — the sound must not jump to neutral on return.
float s_tiltNorm = 0.0f;    // boki = jasność, wygładzone -1..1
float s_tiltFbNorm = 0.0f;  // do/od siebie = przestrzeń
float s_gravX = 0.0f, s_gravY = 0.0f, s_gravZ = 1.0f;  // wolny LP grawitacji
float s_shakeEnv = 0.0f;
bool s_swingArmed = true;
uint32_t s_lastChimeMs = 0;
int s_chimeStep = 0;  // pozycja melodii grzechotki na drabince skali
float s_imuTimer = 0.0f;

// Optional GLIDE is deliberately only a tiny landing gesture, not a
// monophonic synth mode. Fast sequential notes in one physical row may bend
// into each other; a chord pressed in the same keyboard scan still attacks
// cleanly and keeps every voice.
struct RowLanding {
  float cents = 0.0f;
  uint32_t atMs = 0;
  bool valid = false;
};
RowLanding s_rowLanding[4];
// A chord pressed inside one keyboard scan must never read as a glide, so
// both settings keep the same minimum gap. SOFT only catches quick steps
// between neighbours; STRONG waits longer, forgives wider leaps and sings
// the whole way — a deliberate portament rather than a landing.
constexpr uint32_t kGlideMinGapMs = 24;
constexpr uint32_t kSoftGlideMaxGapMs = 180;
constexpr float kSoftGlideMaxJumpCents = 700.0f;
constexpr float kSoftGlideSec = 0.045f;
constexpr uint32_t kStrongGlideMaxGapMs = 420;
constexpr float kStrongGlideMaxJumpCents = 1600.0f;
constexpr float kStrongGlideSec = 0.170f;

gk::PhrasePlayer s_windPhrase;
float s_windVelocity = 0.0f;
}  // namespace

void ModeInstrument::enter(ModeCtx& ctx) {
  settings::applyToEngine(ctx.engine);
  ambient::setCutoffBase(kNeutralCutoff);
  windPhraseCancel(ctx);
  for (auto& landing : s_rowLanding) landing.valid = false;
  s_chimeStep = ga::scaleStepsPerOctave(settings::scale());  // start w środku
  viz::setScene(settings::vizScene());
  // scena mapuje wysokość na X/Y wg realnego zakresu siatki tej skali;
  // najwyższy stopień w oktawie rozciąga zawinięcie X na pełną szerokość.
  // Zakres PRZED viz::reset() — reset odsiewa nasionka z ogrodu i musi już
  // znać zawinięcie oktawy.
  float foldMax = 0.0f;
  for (int col = 0; col < 14; ++col)
    for (int row = 0; row < 4; ++row) {
      const float oct = gridToCents(settings::scale(), col, row) / 1200.0f;
      const float frac = oct - floorf(oct);
      if (frac > foldMax) foldMax = frac;
    }
  viz::setPitchRange(gridToCents(settings::scale(), 0, 0),
                     gridToCents(settings::scale(), 13, 3),
                     foldMax > 0.01f ? foldMax : 1.0f);
  viz::reset();
  // powitanie tylko raz na uruchomienie — potem pudełko już nie instruuje
  // (każdy powrót z ustawień z napisem czytał się jak nagabywanie)
  static bool s_greeted = false;
  if (!s_greeted) {
    s_greeted = true;
    viz::toast(i18n::tr(i18n::TextId::PlayGreeting));
  }
  markDirty();
}

void ModeInstrument::exit(ModeCtx& ctx) {
  windPhraseCancel(ctx);
  ambient::gardenReleaseAll();
  ctx.engine.allNotesOff();
  ambient::setCutoffBase(kNeutralCutoff);
  ambient::setSpaceBase(0.5f);  // głębia też wraca do neutrum, nie tylko jasność
  ctx.engine.setParam(Param::BendCents, 0.0f);
}

void ModeInstrument::onKey(ModeCtx& ctx, int col, int row, bool down) {
  // wszystkie 56 klawiszy gra — kolumna = stopień skali, dolny rząd najniżej
  const int id = row * 14 + col;
  // The physical keyboard is staggered: the digit row sits one key to the
  // right of the letter rows, so the column the EYE sees (e.g. 4-T-F-C) is
  // matrix col+1 on the top row. Tune to the eye, not the matrix — a seen
  // column must stack into one chord. (gridToCents clamps the far corner.)
  const int gridCol = row == 0 ? col + 1 : col;
  if (down) {
    windPhraseCancel(ctx);  // a real key always owns the foreground
    ambient::notePresence();
    pulse::onOnset();  // even presses summon the heart, in YOUR tempo
    const int gridRow = 3 - row;
    const float cents = gridToCents(settings::scale(), gridCol, gridRow);
    const uint32_t nowMs = millis();
    const uint32_t gapMs = nowMs - s_rowLanding[row].atMs;
    const settings::GlideMode mode = settings::glide();
    const bool strong = mode == settings::GlideMode::Strong;
    const uint32_t maxGapMs =
        strong ? kStrongGlideMaxGapMs : kSoftGlideMaxGapMs;
    const float maxJump =
        strong ? kStrongGlideMaxJumpCents : kSoftGlideMaxJumpCents;
    const bool glide = mode != settings::GlideMode::Off &&
                       s_rowLanding[row].valid && gapMs >= kGlideMinGapMs &&
                       gapMs <= maxGapMs &&
                       fabsf(cents - s_rowLanding[row].cents) <= maxJump;
    if (glide)
      ctx.engine.noteOnGlide(id, s_rowLanding[row].cents, cents, 0.9f,
                             strong ? kStrongGlideSec : kSoftGlideSec);
    else
      ctx.engine.noteOn(id, cents, 0.9f);
    s_rowLanding[row] = {cents, nowMs, true};
    ambient::gardenPush(id, cents);
    viz::noteOn(id, cents, 0.9f);
    firefly::note(cents, 0.9f);
  } else {
    ambient::gardenRelease(id);
    ctx.engine.noteOff(id);
    viz::noteOff(id);
  }
}

void ModeInstrument::onGoHold(ModeCtx& ctx) {
  // przytrzymany GO: następna barwa, z dużym napisem zamiast tabelki stanu
  settings::cyclePreset();
  settings::applyToEngine(ctx.engine);
  viz::toast(i18n::presetName(settings::preset()));
}

void ModeInstrument::triggerChime(ModeCtx& ctx, float energy, float dir) {
  // One deliberate swing recalls the latest phrase. Held keys and an
  // ongoing recall own the foreground until their final release.
  if (s_windPhrase.active() || ambient::keysHeld()) return;
  ambient::GardenPhrase phrase;
  if (!ambient::gardenRecall(&phrase)) {
    // pusty ogród: zapasowa drabinka skali, żeby nigdy nie było niemo
    const int hi = 2 * ga::scaleStepsPerOctave(settings::scale());
    s_chimeStep += dir >= 0.0f ? 1 : -1;
    if (s_chimeStep < 0) s_chimeStep = 1;
    if (s_chimeStep > hi) s_chimeStep = hi - 1;
    phrase.count = 1;
    phrase.note[0] = {ga::scaleStepCents(settings::scale(), s_chimeStep), 0};
  }

  s_windVelocity = clampf(0.45f + (energy - kSwingOn) * 0.5f, 0.45f, 0.90f);
  const uint32_t nowMs = millis();
  if (!s_windPhrase.start(phrase, nowMs)) return;
  ambient::notePresence();
  ambient::memoryPlaying(true);
  windPhraseStep(ctx, nowMs);
}

void ModeInstrument::windPhraseStep(ModeCtx& ctx, uint32_t nowMs) {
  const auto batch = s_windPhrase.tick(nowMs);
  for (int i = 0; i < batch.count; ++i) {
    const auto& event = batch.events[i];
    const int32_t id = kChimeIdBase + event.slot;
    if (!event.down) {
      ctx.engine.noteOff(id);
      continue;
    }
    const float vel = s_windVelocity * (float)event.note.velocity / 90.0f;
    // Short articulations need an attack shorter than their captured hold.
    const float attack = fminf(0.06f, event.note.holdMs * 0.00025f);
    ctx.engine.noteOnWithPreset(id, event.note.cents, vel,
                                settings::preset(), attack);
    viz::chime(event.note.cents, vel);
    firefly::note(event.note.cents, vel);
  }
  ambient::memoryPlaying(s_windPhrase.active());
}

void ModeInstrument::windPhraseCancel(ModeCtx& ctx) {
  const auto batch = s_windPhrase.cancel();
  for (int i = 0; i < batch.count; ++i)
    ctx.engine.noteOff(kChimeIdBase + batch.events[i].slot);
  ambient::memoryPlaying(false);
}

void ModeInstrument::imuStep(ModeCtx& ctx) {
  float ax = 0, ay = 0, az = 0;
  M5.Imu.update();
  M5.Imu.getAccel(&ax, &ay, &az);

  // wolny LP wyłuskuje grawitację; reszta to ruch ręki
  const float a = 0.05f;
  s_gravX += (ax - s_gravX) * a;
  s_gravY += (ay - s_gravY) * a;
  s_gravZ += (az - s_gravZ) * a;
  const float lx = ax - s_gravX, ly = ay - s_gravY, lz = az - s_gravZ;
  const float e = sqrtf(lx * lx + ly * ly + lz * lz);
  s_shakeEnv += (e - s_shakeEnv) * 0.25f;

  // DOBRANOC: położone ekranem w dół przez ~1.2 s — ogród
  // śpiewa kołysankę (patrz ambient.h). Podniesienie budzi natychmiast.
  // Detekcja na SUROWYM az: wolny filtr grawitacji potrzebuje czasu, by
  // dogonić obrót, więc nie może bramkować timera (to podwajało deklarowane
  // 1.2 s, a drgania obudowy potrafiły blokować gest jeszcze dłużej).
  // Duszę zapisuje ambient dopiero WE ŚNIE — zapis NVS zatrzymuje rdzeń
  // audio i zgrzytałby w środku muzyki.
  const uint32_t nowMs = millis();
  goodnight::sample(nowMs, az);
  if (ambient::lullabyActive()) return;  // we śnie: bez dzwonków i przechyłów

  // MACHANIE: zamach powyżej progu gra dzwonek; następny dopiero, gdy ruch
  // opadnie (histereza) — jedno machnięcie = jeden dzwonek, nie seria
  const uint32_t now = millis();
  if (s_swingArmed && e > kSwingOn && now - s_lastChimeMs > kSwingCooldownMs) {
    s_swingArmed = false;
    s_lastChimeMs = now;
    // Direction only steers the fallback while the garden is still empty.
    const float dir = fabsf(lx) >= fabsf(ly) ? lx : ly;
    triggerChime(ctx, e, dir);
    goodnight::noteActivity();  // waving the box IS playing it
  } else if (e < kSwingOff) {
    s_swingArmed = true;
  }

  // PRZECHYŁY: liczone z wygładzonej grawitacji, więc machanie nimi nie
  // szarpie; dojeżdżają do celu przez ~0.4 s zamiast skakać
  if (s_shakeEnv < 0.25f) {
    // na boki = jasność brzmienia
    const float tilt = atan2f(-s_gravX, s_gravZ) * 57.2957795f;
    float target = 0.0f;
    const float mag = fabsf(tilt);
    if (mag > kTiltDead)
      target = copysignf(
          fminf((mag - kTiltDead) / (kTiltFull - kTiltDead), 1.0f), tilt);
    s_tiltNorm += (target - s_tiltNorm) * 0.055f;  // tau ~0.4 s przy 50 Hz

    // w stronę ciemna mocno, w stronę jasna delikatnie — neutralnie jest
    // już jasno, więc ekspresja mieszka w przyciemnianiu
    const float cutoff = s_tiltNorm >= 0.0f
                             ? kNeutralCutoff * exp2f(0.65f * s_tiltNorm)
                             : kNeutralCutoff * exp2f(2.4f * s_tiltNorm);
    ambient::setCutoffBase(cutoff);
    viz::setTilt(s_tiltNorm);

    // do/od siebie = głębia przestrzeni: od siebie dźwięk odpływa w pogłos
    // i echo, do siebie robi się suchy i bliski (jak przybliżanie ucha);
    // znak osi to pierwsze przybliżenie — na sprzęcie ew. odwróć s_gravY
    const float tiltFb = atan2f(-s_gravY, s_gravZ) * 57.2957795f;
    float targetFb = 0.0f;
    const float magFb = fabsf(tiltFb);
    if (magFb > kTiltFbDead)
      targetFb = copysignf(
          fminf((magFb - kTiltFbDead) / (kTiltFbFull - kTiltFbDead), 1.0f),
          tiltFb);
    s_tiltFbNorm += (targetFb - s_tiltFbNorm) * 0.055f;
    ambient::setSpaceBase(0.5f + 0.5f * s_tiltFbNorm);
    viz::setDepth(s_tiltFbNorm);  // scena też pokazuje głębię
  }
}

void ModeInstrument::tick(ModeCtx& ctx, float dt) {
  s_imuTimer += dt;
  if (s_imuTimer >= kImuPeriod) {
    s_imuTimer -= kImuPeriod;
    if (s_imuTimer > kImuPeriod) s_imuTimer = 0.0f;
    imuStep(ctx);
  }

  // Recorded onsets and releases, including all notes of a chord in one pass.
  const uint32_t now = millis();
  if (ambient::lullabyActive()) windPhraseCancel(ctx);
  else windPhraseStep(ctx, now);
  // duch zagrał wspomnienie? niech rozbłyśnie jego iskierka na łące
  // (i świetlik w pudełku — miękka poświata zamiast błysku)
  float ghostSource = 0.0f, ghostPlayed = 0.0f;
  while (ambient::pollGhost(&ghostSource, &ghostPlayed)) {
    viz::ghost(ghostSource);
    firefly::ghost(ghostPlayed);
  }

  // łąka żyje cały czas — pełna klatka co przebieg (main ogranicza do ~25 fps)
  markDirty();
}

void ModeInstrument::draw(ModeCtx& ctx) { viz::draw(ctx.canvas); }
