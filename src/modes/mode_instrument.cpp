#include "mode_instrument.h"

#include <M5Unified.h>
#include <math.h>
#include <stdio.h>

#include "../age.h"
#include "../ambient.h"
#include "../hal/audio_out.h"

using namespace ga;

namespace {
const char* kPresetNames[kNumTimbrePresets] = {"PURE",  "DRONE",    "REED",
                                               "CHIME", "MUSICBOX", "VOICE"};
const float kBaseOctaves[4] = {55.0f, 110.0f, 220.0f, 440.0f};
constexpr float kImuPeriod = 0.02f;  // 50 Hz IMU reads
constexpr float kNeutralCutoff = 7500.0f;
// JI ladder for toss landings (flight time -> rung; spin sign -> direction)
const float kRungCents[5] = {203.9f, 386.3f, 702.0f, 968.8f, 1200.0f};
}  // namespace

const char* ModeInstrument::roleName(ImuRole r) {
  switch (r) {
    case ImuRole::Bend:   return "BEND";
    case ImuRole::Filter: return "FILTER";
    default:              return "off";
  }
}

void ModeInstrument::applyRoot(ModeCtx& ctx) {
  const float root =
      kBaseOctaves[octave_] * exp2f(centerCents_ / 1200.0f);
  ctx.engine.setParam(Param::BaseHz, root);
  hal::strings().setRootHz(root);  // the halo bends onto the new center
}

void ModeInstrument::enter(ModeCtx& ctx) {
  if (age::tier() == age::Maluch) {
    // the toddler tier is a fixed, perfect world: pentatonic chime, no knobs
    scale_ = ScaleId::PENTA;
    preset_ = 3;  // CHIME
  } else if (age::tier() == age::Sredniak &&
             scale_ != ScaleId::PENTA && scale_ != ScaleId::MAJOR) {
    scale_ = ScaleId::PENTA;  // only the happy scales at this age
  }
  ctx.engine.setParam(Param::TimbrePreset, (float)preset_);
  ambient::setPreset(preset_);
  applyRoot(ctx);
  ambient::setCutoffBase(kNeutralCutoff);
  ctx.engine.setParam(Param::BendCents, 0.0f);
  held_ = 0;
  toss_ = TossPhase::Grounded;
  freefall_ = 0;
  ctrlHeld_ = ctrlUsed_ = false;
  markDirty();
}

void ModeInstrument::exit(ModeCtx& ctx) {
  ctx.engine.allNotesOff();
  // Return shared engine params to neutral so the next mode does not inherit
  // a tilted filter or bend. The tonal center survives — it is yours.
  ctx.engine.setParam(Param::BendCents, 0.0f);
  ambient::setCutoffBase(kNeutralCutoff);
}

void ModeInstrument::onKey(ModeCtx& ctx, int col, int row, bool down) {
  if (down) ambient::notePresence();

  if (age::tier() == age::Maluch) {
    // MALUCH: all 56 keys play pentatonic — the control column too; there
    // is nothing to switch, nothing to get lost in, no wrong key anywhere
    const int id = row * 14 + col;
    const uint64_t m = (uint64_t)1 << id;
    if (down) {
      const int gridRow = 3 - row;
      const float cents = gridToCents(ScaleId::PENTA, col, gridRow);
      ctx.engine.noteOn(id, cents, 0.9f);
      ambient::gardenPush(cents);
      held_ |= m;
    } else {
      ctx.engine.noteOff(id);
      held_ &= ~m;
    }
    markDirty();
    return;
  }

  if (col == 0) {  // control column
    if (row == 3) {
      // TAP = octave cycle, HOLD + grid = background pick
      if (down) {
        ctrlHeld_ = true;
        ctrlUsed_ = false;
      } else {
        if (ctrlHeld_ && !ctrlUsed_) {
          octave_ = (octave_ + 1) % 4;
          applyRoot(ctx);
        }
        ctrlHeld_ = false;
        markDirty();
      }
      return;
    }
    if (!down) return;
    switch (row) {
      case 0:
        if (age::tier() == age::Sredniak) {
          // only the happy scales at this age
          scale_ = scale_ == ScaleId::PENTA ? ScaleId::MAJOR : ScaleId::PENTA;
        } else {
          scale_ = (ScaleId)(((int)scale_ + 1) % (int)ScaleId::Count);
        }
        ctx.engine.allNotesOff();  // clean retuning
        ambient::backgroundSetEnabled(ambient::backgroundEnabled());
        break;
      case 1:
        preset_ = (preset_ + 1) % kNumTimbrePresets;
        // VOICE only exists once a grain is caught (device mic path: v2)
        if (preset_ == 5 && !ctx.engine.hasVoiceSample()) preset_ = 0;
        ctx.engine.setParam(Param::TimbrePreset, (float)preset_);
        ambient::setPreset(preset_);
        break;
      case 2:
        imuRole_ = (ImuRole)(((int)imuRole_ + 1) % 3);
        // back to neutral when the role changes
        ctx.engine.setParam(Param::BendCents, 0.0f);
        ambient::setCutoffBase(kNeutralCutoff);
        imuShown_ = 0.0f;
        break;
    }
    markDirty();
    return;
  }

  // playing keys: col 1..13 -> grid column 0..12
  const int gridRow = 3 - row;  // bottom physical row = lowest interval
  const float cents = gridToCents(scale_, col - 1, gridRow);

  if (ctrlHeld_) {
    // HOLD ctrl + grid key = toggle this pitch in the background chord
    if (down) {
      ambient::backgroundToggleNote(cents);
      ctrlUsed_ = true;
      markDirty();
    }
    return;
  }

  const int id = row * 14 + col;
  const uint64_t m = (uint64_t)1 << id;
  if (down) {
    ctx.engine.noteOn(id, cents, 0.9f);
    ambient::gardenPush(cents);
    held_ |= m;
  } else {
    ctx.engine.noteOff(id);
    held_ &= ~m;
  }
  markDirty();
}

void ModeInstrument::applyTilt(ModeCtx& ctx, float ax, float az) {
  // Case tilt; the axis is a first guess — confirm on hardware
  // (if the wrong tilt direction responds, swap ax for ay below).
  const float tilt = atan2f(-ax, az) * 57.2957795f;
  float norm = 0.0f;
  const float a = fabsf(tilt);
  if (a > 6.0f)  // dead zone so resting jitter does not detune
    norm = copysignf(fminf((a - 6.0f) / 34.0f, 1.0f), tilt);

  float shown = imuShown_;
  switch (imuRole_) {
    case ImuRole::Bend:
      shown = norm * 200.0f;  // ±200 cents of glissando
      ctx.engine.setParam(Param::BendCents, shown);
      if (fabsf(shown - imuShown_) > 4.0f) markDirty();
      break;
    case ImuRole::Filter:
      shown = 1500.0f * exp2f(norm * 2.2f);  // ~330..6900 Hz
      // the weather is the single writer of the cutoff — we move its base
      ambient::setCutoffBase(shown);
      if (fabsf(shown - imuShown_) > imuShown_ * 0.06f + 10.0f) markDirty();
      break;
    case ImuRole::Off:
      return;
  }
  imuShown_ = shown;
}

void ModeInstrument::land(ModeCtx& ctx, float flightSec) {
  toss_ = TossPhase::Grounded;
  freefall_ = 0;
  if (flightSec < 0.12f) {  // a bump, not a throw
    ctx.engine.setParam(Param::BendCents, 0.0f);
    return;
  }
  int rung;
  if (flightSec < 0.35f) rung = 1;
  else if (flightSec < 0.50f) rung = 2;
  else if (flightSec < 0.65f) rung = 3;
  else if (flightSec < 0.80f) rung = 4;
  else rung = 5;
  // spin direction picks up vs down; the gyro sign convention is a first
  // guess — if it lands the wrong way on hardware, flip the comparison
  const float dir =
      (fabsf(spinDeg_) > 120.0f && spinDeg_ < 0.0f) ? -1.0f : 1.0f;
  lastLandCents_ = dir * kRungCents[rung - 1];
  centerCents_ = clampf(centerCents_ + lastLandCents_, -2400.0f, 2400.0f);
  applyRoot(ctx);
  ctx.engine.setParam(Param::BendCents, 0.0f);
  markDirty();
}

void ModeInstrument::tick(ModeCtx& ctx, float dt) {
  imuTimer_ += dt;
  if (imuTimer_ >= kImuPeriod) {
    imuTimer_ -= kImuPeriod;
    if (imuTimer_ > kImuPeriod) imuTimer_ = 0.0f;

    float ax = 0, ay = 0, az = 0;
    M5.Imu.update();
    M5.Imu.getAccel(&ax, &ay, &az);
    const float mag = sqrtf(ax * ax + ay * ay + az * az);

    if (toss_ == TossPhase::Grounded) {
      if (mag < 0.35f) {  // free fall: the accelerometer reads ~0 g
        if (++freefall_ >= 2) {
          toss_ = TossPhase::Flight;
          tossT0Ms_ = millis();
          spinDeg_ = 0.0f;
        }
      } else {
        freefall_ = 0;
        applyTilt(ctx, ax, az);
      }
    } else {  // Flight
      float gx = 0, gy = 0, gz = 0;
      M5.Imu.getGyro(&gx, &gy, &gz);
      spinDeg_ += gz * kImuPeriod;  // deg/s * s
      const float t = (float)(millis() - tossT0Ms_) * 0.001f;
      float alt = 620.0f * t;
      if (alt > 1250.0f) alt = 1250.0f;
      ctx.engine.setParam(Param::BendCents, alt);
      if (mag > 1.6f) {
        land(ctx, t);
      } else if (t > 3.0f) {  // lost the catch: give up gracefully
        toss_ = TossPhase::Grounded;
        ctx.engine.setParam(Param::BendCents, 0.0f);
      }
    }
  }
  const int v = ctx.engine.activeVoiceCount();
  if (v != lastVoices_) {
    lastVoices_ = v;
    markDirty();
  }
}

void ModeInstrument::draw(ModeCtx& ctx) {
  auto& g = ctx.canvas;
  g.fillSprite(TFT_BLACK);
  g.setTextSize(1);
  g.setTextColor(TFT_WHITE, TFT_BLACK);

  char buf[64];
  snprintf(buf, sizeof buf, "%s | %s | %d Hz | ctr %+d c | %s",
           scaleInfo(scale_).name, kPresetNames[preset_],
           (int)kBaseOctaves[octave_], (int)centerCents_,
           age::label(age::tier()));
  g.drawString(buf, 4, 3);
  g.drawFastHLine(0, 14, 240, TFT_DARKGREY);

  // 13x4 grid; screen row 0 = the top physical row (row 0)
  for (int row = 0; row < 4; ++row) {
    for (int col = 1; col <= 13; ++col) {
      const int x = 8 + (col - 1) * 17;
      const int y = 20 + row * 17;
      const bool on = (held_ >> (row * 14 + col)) & 1;
      if (on)
        g.fillRoundRect(x, y, 15, 15, 2, TFT_ORANGE);
      else
        g.drawRoundRect(x, y, 15, 15, 2, TFT_DARKGREY);
    }
  }

  g.drawFastHLine(0, 92, 240, TFT_DARKGREY);
  g.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  if (toss_ == TossPhase::Flight)
    g.drawString("LOT... zlap mnie", 4, 97);
  else if (ctrlHeld_)
    g.drawString("TLO: nacisnij nuty (ctrl trzymasz)", 4, 97);
  else
    g.drawString("` skala  tab barwa  fn IMU  ctrl okt/TLO", 4, 97);

  if (imuRole_ == ImuRole::Bend)
    snprintf(buf, sizeof buf, "IMU %s %+d c  tlo %d  glosy %d  GO=menu",
             roleName(imuRole_), (int)imuShown_, ambient::backgroundCount(),
             lastVoices_);
  else if (imuRole_ == ImuRole::Filter)
    snprintf(buf, sizeof buf, "IMU %s %d Hz  tlo %d  glosy %d  GO=menu",
             roleName(imuRole_), (int)imuShown_, ambient::backgroundCount(),
             lastVoices_);
  else
    snprintf(buf, sizeof buf, "IMU %s  tlo %d  glosy %d  GO=menu",
             roleName(imuRole_), ambient::backgroundCount(), lastVoices_);
  g.setTextColor(TFT_WHITE, TFT_BLACK);
  g.drawString(buf, 4, 122);
}
