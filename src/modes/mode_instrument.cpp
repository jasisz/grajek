#include "mode_instrument.h"

#include <M5Unified.h>
#include <math.h>

#include "../ambient.h"
#include "../settings.h"
#include "../viz.h"
#include "ga_dsp.h"
#include "ga_scales.h"

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
}  // namespace

void ModeInstrument::enter(ModeCtx& ctx) {
  settings::applyToEngine(ctx.engine);
  ambient::setCutoffBase(kNeutralCutoff);
  held_ = 0;
  tiltNorm_ = 0.0f;
  shakeEnv_ = 0.0f;
  swingArmed_ = true;
  pendingCount_ = 0;
  chimeStep_ = ga::scaleStepsPerOctave(settings::scale());  // start w środku
  viz::setScene(settings::vizScene());
  viz::reset();
  // scena mapuje wysokość na X/Y wg realnego zakresu siatki tej skali;
  // najwyższy stopień w oktawie rozciąga zawinięcie X na pełną szerokość
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
  viz::toast("graj!");  // potwierdzenie wejścia — ŚPIEW nadpisze swoim
  markDirty();
}

void ModeInstrument::exit(ModeCtx& ctx) {
  ctx.engine.allNotesOff();
  ambient::setCutoffBase(kNeutralCutoff);
  ctx.engine.setParam(Param::BendCents, 0.0f);
}

void ModeInstrument::onKey(ModeCtx& ctx, int col, int row, bool down) {
  // wszystkie 56 klawiszy gra — kolumna = stopień skali, dolny rząd najniżej
  const int id = row * 14 + col;
  const uint64_t m = (uint64_t)1 << id;
  if (down) {
    ambient::notePresence();
    const int gridRow = 3 - row;
    const float cents = gridToCents(settings::scale(), col, gridRow);
    ctx.engine.noteOn(id, cents, 0.9f);
    ambient::gardenPush(cents);
    viz::noteOn(id, cents, 0.9f);
    held_ |= m;
  } else {
    ctx.engine.noteOff(id);
    viz::noteOff(id);
    held_ &= ~m;
  }
}

void ModeInstrument::onGoHold(ModeCtx& ctx) {
  // przytrzymany GO: następna barwa, z dużym napisem zamiast tabelki stanu
  settings::cyclePreset();
  settings::applyToEngine(ctx.engine);
  viz::toast(settings::presetName(settings::preset()));
}

void ModeInstrument::triggerChime(ModeCtx& ctx, float energy, float dir) {
  // MACHANIE = WIATR WSPOMNIEŃ: klawisze robią nowe nuty, potrząsanie gra
  // stare — każdy zamach wyrywa z ogrodu nutkę, którą dziecko naprawdę
  // zagrało. Im więcej grasz, tym bogatsza grzechotka; ruch nagradza
  // klawisze zamiast z nimi konkurować.
  float cents;
  if (!ambient::gardenPluck(&cents)) {
    // pusty ogród: zapasowa drabinka skali, żeby nigdy nie było niemo
    const int hi = 2 * ga::scaleStepsPerOctave(settings::scale());
    chimeStep_ += dir >= 0.0f ? 1 : -1;
    if (chimeStep_ < 0) chimeStep_ = 1;
    if (chimeStep_ > hi) chimeStep_ = hi - 1;
    cents = ga::scaleStepCents(settings::scale(), chimeStep_);
  }

  const float vel = clampf(0.35f + (energy - kSwingOn) * 0.45f, 0.35f, 0.85f);
  const int32_t id = kChimeIdBase + (chimeSlot_++ & 7);
  // miękki atak tylko dla tej nuty (kanapka FIFO jak u duchów) — wiatr
  // unosi wspomnienia, nie uderza w nie
  ctx.engine.setParam(Param::EnvAttack, 0.25f);
  ctx.engine.noteOn(id, cents, vel);
  ctx.engine.setParam(Param::EnvAttack,
                      ga::timbrePreset(settings::preset()).attack);
  if (pendingCount_ < 8)
    pending_[pendingCount_++] = {millis() + 150, id};

  ambient::notePresence();
  viz::chime(cents, vel);
}

void ModeInstrument::imuStep(ModeCtx& ctx) {
  float ax = 0, ay = 0, az = 0;
  M5.Imu.update();
  M5.Imu.getAccel(&ax, &ay, &az);

  // wolny LP wyłuskuje grawitację; reszta to ruch ręki
  const float a = 0.05f;
  gravX_ += (ax - gravX_) * a;
  gravY_ += (ay - gravY_) * a;
  gravZ_ += (az - gravZ_) * a;
  const float lx = ax - gravX_, ly = ay - gravY_, lz = az - gravZ_;
  const float e = sqrtf(lx * lx + ly * ly + lz * lz);
  shakeEnv_ += (e - shakeEnv_) * 0.25f;

  // MACHANIE: zamach powyżej progu gra dzwonek; następny dopiero, gdy ruch
  // opadnie (histereza) — jedno machnięcie = jeden dzwonek, nie seria
  const uint32_t now = millis();
  if (swingArmed_ && e > kSwingOn && now - lastChimeMs_ > kSwingCooldownMs) {
    swingArmed_ = false;
    lastChimeMs_ = now;
    // kierunek zamachu wzdłuż dominującej osi — machanie w lewo/prawo
    // prowadzi melodię w dół/górę
    const float dir = fabsf(lx) >= fabsf(ly) ? lx : ly;
    triggerChime(ctx, e, dir);
  } else if (e < kSwingOff) {
    swingArmed_ = true;
  }

  // PRZECHYŁY: liczone z wygładzonej grawitacji, więc machanie nimi nie
  // szarpie; dojeżdżają do celu przez ~0.4 s zamiast skakać
  if (shakeEnv_ < 0.25f) {
    // na boki = jasność brzmienia
    const float tilt = atan2f(-gravX_, gravZ_) * 57.2957795f;
    float target = 0.0f;
    const float mag = fabsf(tilt);
    if (mag > kTiltDead)
      target = copysignf(
          fminf((mag - kTiltDead) / (kTiltFull - kTiltDead), 1.0f), tilt);
    tiltNorm_ += (target - tiltNorm_) * 0.055f;  // tau ~0.4 s przy 50 Hz

    // w stronę ciemna mocno, w stronę jasna delikatnie — neutralnie jest
    // już jasno, więc ekspresja mieszka w przyciemnianiu
    const float cutoff = tiltNorm_ >= 0.0f
                             ? kNeutralCutoff * exp2f(0.65f * tiltNorm_)
                             : kNeutralCutoff * exp2f(2.4f * tiltNorm_);
    ambient::setCutoffBase(cutoff);
    viz::setTilt(tiltNorm_);

    // do/od siebie = głębia przestrzeni: od siebie dźwięk odpływa w pogłos
    // i echo, do siebie robi się suchy i bliski (jak przybliżanie ucha);
    // znak osi to pierwsze przybliżenie — na sprzęcie ew. odwróć gravY_
    const float tiltFb = atan2f(-gravY_, gravZ_) * 57.2957795f;
    float targetFb = 0.0f;
    const float magFb = fabsf(tiltFb);
    if (magFb > kTiltFbDead)
      targetFb = copysignf(
          fminf((magFb - kTiltFbDead) / (kTiltFbFull - kTiltFbDead), 1.0f),
          tiltFb);
    tiltFbNorm_ += (targetFb - tiltFbNorm_) * 0.055f;
    ambient::setSpaceBase(0.5f + 0.5f * tiltFbNorm_);
    viz::setDepth(tiltFbNorm_);  // scena też pokazuje głębię
  }
}

void ModeInstrument::tick(ModeCtx& ctx, float dt) {
  imuTimer_ += dt;
  if (imuTimer_ >= kImuPeriod) {
    imuTimer_ -= kImuPeriod;
    if (imuTimer_ > kImuPeriod) imuTimer_ = 0.0f;
    imuStep(ctx);
  }

  // zaplanowane wyciszenia dzwonków (nuta krótka, wybrzmiewa release'em)
  const uint32_t now = millis();
  for (int i = 0; i < pendingCount_;) {
    if ((int32_t)(now - pending_[i].atMs) >= 0) {
      ctx.engine.noteOff(pending_[i].id);
      pending_[i] = pending_[--pendingCount_];
    } else {
      ++i;
    }
  }

  // duch zagrał wspomnienie? niech rozbłyśnie jego iskierka na łące
  float ghostCents = 0.0f;
  if (ambient::pollGhost(&ghostCents)) viz::ghost(ghostCents);

  // łąka żyje cały czas — pełna klatka co przebieg (main ogranicza do ~25 fps)
  markDirty();
}

void ModeInstrument::draw(ModeCtx& ctx) { viz::draw(ctx.canvas); }
