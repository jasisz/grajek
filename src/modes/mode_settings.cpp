#include "mode_settings.h"

#include <M5Unified.h>
#include <stdio.h>

#include "../ambient.h"
#include "../firefly.h"
#include "../goodnight.h"
#include "../settings.h"
#include "../soul.h"
#include "../viz.h"
#include "ga_scales.h"

void ModeSettings::enter(ModeCtx&) {
  // A normal settings visit is a natural pause. During goodnight, NVS must
  // wait for ambient's scheduled save in real silence.
  if (!ambient::lullabyActive()) soul::save();
  imuTimer_ = 0.0f;
  markDirty();
}

void ModeSettings::exit(ModeCtx&) { settings::save(); }

void ModeSettings::onKey(ModeCtx& ctx, int col, int row, bool down) {
  if (!down) return;
  // Any physical key is an explicit wake-up even on the settings screen.
  // It is not musical presence: restored phrases must still wait for play.
  if (row != 0) return;  // cyfry mieszkają w górnym rzędzie
  switch (col) {
    case 1: settings::cycleScale(); break;
    case 2:
      settings::cyclePreset();
      settings::applyToEngine(ctx.engine);
      break;
    case 3:
      settings::cycleOctave();
      settings::applyToEngine(ctx.engine);
      break;
    case 4: settings::toggleBackground(); break;
    case 5: settings::cycleVizScene(); break;
    default: return;
  }
  markDirty();
}

void ModeSettings::tick(ModeCtx&, float dt) {
  const uint32_t nowMs = millis();
  float ghostSource = 0.0f, ghostPlayed = 0.0f;
  if (ambient::pollGhost(&ghostSource, &ghostPlayed))
    firefly::ghost(ghostPlayed);  // drain the event outside the play scenes

  constexpr float kImuPeriod = 0.02f;
  imuTimer_ += dt;
  if (imuTimer_ < kImuPeriod) return;
  imuTimer_ -= kImuPeriod;
  if (imuTimer_ > kImuPeriod) imuTimer_ = 0.0f;

  float ax = 0.0f, ay = 0.0f, az = 0.0f;
  M5.Imu.update();
  M5.Imu.getAccel(&ax, &ay, &az);
  const bool wasSleeping = ambient::lullabyActive();
  goodnight::sample(nowMs, az);
  if (!wasSleeping && ambient::lullabyActive()) {
    // The first lullaby note waits 1.2 s, so this is a natural silent point
    // to persist settings even if the box is powered off without leaving the
    // settings screen. A failed write remains dirty and ambient retries it
    // after the lullaby reaches complete silence.
    settings::save();
  }
}

void ModeSettings::draw(ModeCtx& ctx) {
  auto& g = ctx.canvas;
  g.fillSprite(TFT_BLACK);
  char buf[32];

  g.setTextSize(2);
  g.setTextColor(TFT_ORANGE, TFT_BLACK);
  g.drawString("ustawienia", 6, 4);
  g.setTextSize(1);
  g.setTextColor(TFT_DARKGREY, TFT_BLACK);
  g.drawString("GO = graj", 172, 12);

  // krótkie etykiety: przy rozmiarze 2 kolumna wartości startuje na 100 px,
  // a najdłuższa nazwa skali ("JI 11-limit") kończy się dokładnie w kadrze
  const char* labels[5] = {"1 skala", "2 barwa", "3 okt.", "4 tlo",
                           "5 scena"};
  const char* values[5];
  values[0] = ga::scaleInfo(settings::scale()).name;
  values[1] = settings::presetName(settings::preset());
  snprintf(buf, sizeof buf, "%d Hz", (int)settings::baseHz());
  values[2] = buf;
  values[3] = settings::backgroundOn() ? "gra" : "cicho";
  values[4] = viz::sceneName(settings::vizScene());

  g.setTextSize(2);
  for (int i = 0; i < 5; ++i) {
    const int y = 26 + i * 17;
    g.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    g.drawString(labels[i], 6, y);
    g.setTextColor(TFT_WHITE, TFT_BLACK);
    g.drawString(values[i], 100, y);
  }
}
