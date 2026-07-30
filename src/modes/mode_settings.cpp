#include "mode_settings.h"

#include <stdio.h>

#include "../settings.h"
#include "../viz.h"
#include "ga_scales.h"

void ModeSettings::enter(ModeCtx&) { markDirty(); }

void ModeSettings::onKey(ModeCtx& ctx, int col, int row, bool down) {
  if (!down || row != 0) return;  // cyfry mieszkają w górnym rzędzie
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
    case 6: settings::toggleSing(); break;
    default: return;
  }
  markDirty();
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
  const char* labels[6] = {"1 skala", "2 barwa", "3 okt.",
                           "4 tlo",   "5 scena", "6 ucho"};
  const char* values[6];
  values[0] = ga::scaleInfo(settings::scale()).name;
  values[1] = settings::presetName(settings::preset());
  snprintf(buf, sizeof buf, "%d Hz", (int)settings::baseHz());
  values[2] = buf;
  values[3] = settings::backgroundOn() ? "gra" : "cicho";
  values[4] = viz::sceneName(settings::vizScene());
  values[5] = settings::singOn() ? "sluchaj" : "nie";

  g.setTextSize(2);
  for (int i = 0; i < 6; ++i) {
    const int y = 26 + i * 17;
    g.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    g.drawString(labels[i], 6, y);
    g.setTextColor(TFT_WHITE, TFT_BLACK);
    g.drawString(values[i], 100, y);
  }
}
