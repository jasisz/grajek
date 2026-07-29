#include "mode_instrument.h"

#include <M5Unified.h>
#include <math.h>
#include <stdio.h>

using namespace ga;

namespace {
const char* kPresetNames[kNumTimbrePresets] = {"PURE", "DRONE", "REED",
                                               "CHIME"};
const float kBaseOctaves[4] = {55.0f, 110.0f, 220.0f, 440.0f};
constexpr float kImuPeriod = 0.02f;  // 50 Hz IMU reads
constexpr float kNeutralCutoff = 6000.0f;
}  // namespace

const char* ModeInstrument::roleName(ImuRole r) {
  switch (r) {
    case ImuRole::Bend:   return "BEND";
    case ImuRole::Filter: return "FILTER";
    default:              return "off";
  }
}

void ModeInstrument::enter(ModeCtx& ctx) {
  ctx.engine.setParam(Param::TimbrePreset, (float)preset_);
  ctx.engine.setParam(Param::BaseHz, kBaseOctaves[octave_]);
  ctx.engine.setParam(Param::FilterCutoffHz, kNeutralCutoff);
  ctx.engine.setParam(Param::BendCents, 0.0f);
  held_ = 0;
  markDirty();
}

void ModeInstrument::exit(ModeCtx& ctx) {
  ctx.engine.allNotesOff();
  // Return shared engine params to neutral so the next mode does not inherit
  // a tilted filter or bend.
  ctx.engine.setParam(Param::BendCents, 0.0f);
  ctx.engine.setParam(Param::FilterCutoffHz, kNeutralCutoff);
}

void ModeInstrument::onKey(ModeCtx& ctx, int col, int row, bool down) {
  if (col == 0) {  // control column
    if (!down) return;
    switch (row) {
      case 0:
        scale_ = (ScaleId)(((int)scale_ + 1) % (int)ScaleId::Count);
        ctx.engine.allNotesOff();  // clean retuning
        break;
      case 1:
        preset_ = (preset_ + 1) % kNumTimbrePresets;
        ctx.engine.setParam(Param::TimbrePreset, (float)preset_);
        break;
      case 2:
        imuRole_ = (ImuRole)(((int)imuRole_ + 1) % 3);
        // back to neutral when the role changes
        ctx.engine.setParam(Param::BendCents, 0.0f);
        ctx.engine.setParam(Param::FilterCutoffHz, kNeutralCutoff);
        imuShown_ = 0.0f;
        break;
      case 3:
        octave_ = (octave_ + 1) % 4;
        ctx.engine.setParam(Param::BaseHz, kBaseOctaves[octave_]);
        break;
    }
    markDirty();
    return;
  }

  // playing keys: col 1..13 -> grid column 0..12
  const int id = row * 14 + col;
  const uint64_t m = (uint64_t)1 << id;
  if (down) {
    const int gridRow = 3 - row;  // bottom physical row = lowest interval
    ctx.engine.noteOn(id, gridToCents(scale_, col - 1, gridRow), 0.9f);
    held_ |= m;
  } else {
    ctx.engine.noteOff(id);
    held_ &= ~m;
  }
  markDirty();
}

void ModeInstrument::applyImu(ModeCtx& ctx) {
  float ax = 0, ay = 0, az = 0;
  M5.Imu.update();
  M5.Imu.getAccel(&ax, &ay, &az);
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
      ctx.engine.setParam(Param::FilterCutoffHz, shown);
      if (fabsf(shown - imuShown_) > imuShown_ * 0.06f + 10.0f) markDirty();
      break;
    case ImuRole::Off:
      return;
  }
  imuShown_ = shown;
}

void ModeInstrument::tick(ModeCtx& ctx, float dt) {
  imuTimer_ += dt;
  if (imuTimer_ >= kImuPeriod) {
    imuTimer_ -= kImuPeriod;  // keep the remainder — no drift below 50 Hz
    if (imuTimer_ > kImuPeriod) imuTimer_ = 0.0f;  // don't accumulate a backlog
    applyImu(ctx);
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
  snprintf(buf, sizeof buf, "%s  |  %s  |  base %d Hz",
           scaleInfo(scale_).name, kPresetNames[preset_],
           (int)kBaseOctaves[octave_]);
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
  g.drawString("left col: scale / timbre / IMU / octave", 4, 97);

  if (imuRole_ == ImuRole::Bend)
    snprintf(buf, sizeof buf, "IMU %s %+d c   voices %d   GO=menu",
             roleName(imuRole_), (int)imuShown_, lastVoices_);
  else if (imuRole_ == ImuRole::Filter)
    snprintf(buf, sizeof buf, "IMU %s %d Hz   voices %d   GO=menu",
             roleName(imuRole_), (int)imuShown_, lastVoices_);
  else
    snprintf(buf, sizeof buf, "IMU %s   voices %d   GO=menu",
             roleName(imuRole_), lastVoices_);
  g.setTextColor(TFT_WHITE, TFT_BLACK);
  g.drawString(buf, 4, 122);
}
