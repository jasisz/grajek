// grajek — an experimental instrument on the M5Stack Cardputer-ADV.
//
// Core split:
//   core 0: audio task (hal/audio_out.cpp) — synthesis + I2S, nothing blocks it
//   core 1: Arduino loop — keyboard, IMU, LCD, mode logic
// The two sides talk exclusively through the engine's lock-free event queue.
#include <M5Cardputer.h>

#include "ga_engine.h"
#include "hal/audio_out.h"
#include "input/keys.h"
#include "modes/mode.h"
#include "modes/mode_instrument.h"
#include "modes/mode_stub.h"

namespace {

ga::Engine engine;
M5Canvas canvas(&M5Cardputer.Display);

ModeInstrument modeInstrument;
ModeStub modeLoop("LOOP", "layered looper fed by the microphone");
ModeStub modeDrone("DRONE", "plays itself when lying flat");
ModeStub modeIr("IR CONDUCTOR", "IR command sequencer for AV gear");
Mode* modes[4] = {&modeInstrument, &modeLoop, &modeDrone, &modeIr};

int currentMode = -1;  // -1 = menu
bool menuDirty = true;
bool audioOk = false;
uint32_t lastFrameMs = 0;
uint32_t lastTickMs = 0;

void drawMenu() {
  canvas.fillSprite(TFT_BLACK);
  canvas.setTextSize(2);
  canvas.setTextColor(TFT_ORANGE, TFT_BLACK);
  canvas.drawString("grajek", 8, 6);
  canvas.setTextSize(1);
  canvas.setTextColor(TFT_WHITE, TFT_BLACK);
  for (int i = 0; i < 4; ++i) {
    char line[48];
    snprintf(line, sizeof line, "%d  %s%s", i + 1, modes[i]->name(),
             i == 0 ? "" : "  (soon)");
    canvas.drawString(line, 16, 36 + i * 16);
  }
  canvas.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  canvas.drawString("press 1-4 to pick a mode", 8, 108);
  if (!audioOk) {
    canvas.setTextColor(TFT_RED, TFT_BLACK);
    canvas.drawString("AUDIO INIT FAILED - check serial log", 8, 122);
  } else {
    canvas.drawString("GO returns here from any mode", 8, 122);
  }
  canvas.pushSprite(0, 0);
}

void enterMode(ModeCtx& ctx, int idx) {
  currentMode = idx;
  modes[idx]->enter(ctx);
}

void leaveMode(ModeCtx& ctx) {
  modes[currentMode]->exit(ctx);
  engine.allNotesOff();
  currentMode = -1;
  menuDirty = true;
}

}  // namespace

void setup() {
  auto cfg = M5.config();
  cfg.internal_spk = false;  // we drive the codec ourselves (low latency)
  cfg.internal_mic = false;  // mic comes later with the LOOP mode
  cfg.internal_imu = true;
  M5Cardputer.begin(cfg, true);
  M5Cardputer.Display.setRotation(1);
  M5Cardputer.Display.setBrightness(160);

  canvas.setPsram(false);  // StampS3A has no PSRAM — allocate in internal RAM
  canvas.setColorDepth(16);
  if (!canvas.createSprite(240, 135))
    Serial.println("grajek: canvas allocation FAILED");

  engine.init(48000.0f);
  engine.setParam(ga::Param::FilterCutoffHz, 6000.0f);

  audioOk = hal::audioInit(&engine);
  if (!audioOk) Serial.println("grajek: audio init FAILED");

  input::keysInit();
  lastTickMs = millis();
}

void loop() {
  // dt measured at the top of the pass so draw/pushSprite time is included
  const uint32_t now = millis();
  const float dt = (float)(now - lastTickMs) / 1000.0f;
  lastTickMs = now;

  M5Cardputer.update();

  input::KeyEvent ev[input::kMaxKeyEvents];
  const int n = input::keysPoll(ev, input::kMaxKeyEvents);
  ModeCtx ctx{engine, canvas};

  if (input::goPressed() && currentMode >= 0) leaveMode(ctx);

  if (currentMode < 0) {
    for (int i = 0; i < n; ++i) {
      // digits 1..4 live in the top row only
      if (ev[i].down && ev[i].row == 0 && ev[i].col >= 1 && ev[i].col <= 4) {
        enterMode(ctx, ev[i].col - 1);
        break;
      }
    }
  }

  if (currentMode < 0) {
    if (menuDirty) {
      drawMenu();
      menuDirty = false;
    }
  } else {
    Mode* m = modes[currentMode];
    for (int i = 0; i < n; ++i) m->onKey(ctx, ev[i].col, ev[i].row, ev[i].down);
    m->tick(ctx, dt);

    if (m->dirty() && now - lastFrameMs >= 40) {  // ~25 fps max
      m->draw(ctx);
      canvas.pushSprite(0, 0);
      m->clearDirty();
      lastFrameMs = now;
    }
  }

  delay(2);  // breathing room for WDT/USB; audio lives on the other core anyway
}
