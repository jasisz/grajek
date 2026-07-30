// grajek — an experimental instrument on the M5Stack Cardputer-ADV.
//
// Core split:
//   core 0: audio task (hal/audio_out.cpp) — synthesis + I2S, nothing blocks it
//   core 1: Arduino loop — keyboard, IMU, LCD, mode logic
// The two sides talk exclusively through the engine's lock-free event queue.
#include <M5Cardputer.h>

#include "ambient.h"
#include "ga_engine.h"
#include "hal/audio_out.h"
#include "input/keys.h"
#include "modes/mode.h"
#include "modes/mode_instrument.h"
#include "modes/mode_settings.h"
#include "modes/mode_sing.h"
#include "pulse.h"
#include "settings.h"
#include "soul.h"

namespace {

ga::Engine engine;
// NO parent at global-construction time: M5Cardputer.Display is a REFERENCE
// member initialized by the library's constructor, and cross-TU global
// construction order is unspecified — binding it here captured a null parent
// and the first pushSprite crashed the core (the boot-loop on first
// hardware contact). The push destination is passed explicitly instead.
M5Canvas canvas;

// Bez menu: pudełko albo GRA, albo pokazuje USTAWIENIA. Krótkie GO
// przełącza między tymi dwoma światami, przytrzymane (podczas grania)
// zmienia barwę. Granie to INSTRUMENT, a z otwartym uchem — ŚPIEW
// (przełącznik „6 ucho" w ustawieniach).
ModeInstrument modeInstrument;
ModeSing modeSing;
ModeSettings modeSettings;

Mode* current = nullptr;
bool inSettings = false;
bool audioOk = false;
bool displayOk = false;  // M5GFX autodetect can fail — never draw blind:
                         // pushSprite on a panel-less display hard-crashes
uint32_t lastFrameMs = 0;
uint32_t lastTickMs = 0;

// GO: krótkie puszczenie = powrót do menu, przytrzymanie = akcja trybu
// (w INSTRUMENT: następna barwa), powtarzana co 700 ms póki trzymany
bool goLongUsed = false;
uint32_t goNextActionMs = 600;

void switchTo(ModeCtx& ctx, Mode* next) {
  if (current) {
    current->exit(ctx);
    engine.allNotesOff();
    ambient::backgroundRefresh();  // czystka nie może zabijać tampury
  }
  current = next;
  Serial.printf("grajek: -> %s\n", next->name());
  next->enter(ctx);
}

// granie: INSTRUMENT albo ŚPIEW, wedle przełącznika w ustawieniach
Mode* playMode() {
  return settings::singOn() ? (Mode*)&modeSing : (Mode*)&modeInstrument;
}

}  // namespace

void setup() {
  Serial.begin(115200);  // our diagnostics were silently dropped without it
  auto cfg = M5.config();
  cfg.internal_spk = false;  // we drive the codec ourselves (low latency)
  cfg.internal_mic = true;   // configures M5.Mic pins + the ES8311 ADC
                             // callback; nothing starts until a listening
                             // window borrows the bus (see audio_out.h)
  cfg.internal_imu = true;
  M5Cardputer.begin(cfg, true);
  // Diagnose the display before touching it: if the M5GFX autodetect did
  // not bind a panel, every pushSprite is a hard crash (boot loop).
  displayOk = M5Cardputer.Display.getPanel() != nullptr &&
              M5Cardputer.Display.width() > 0;
  Serial.printf("grajek: board=%d display=%dx%d panel=%p -> %s\n",
                (int)M5.getBoard(), M5Cardputer.Display.width(),
                M5Cardputer.Display.height(),
                (void*)M5Cardputer.Display.getPanel(),
                displayOk ? "OK" : "NO PANEL (autodetect failed)");
  if (displayOk) {
    M5Cardputer.Display.setRotation(1);
    M5Cardputer.Display.setBrightness(160);
  }

  if (displayOk) {
    canvas.setPsram(false);  // StampS3A has no PSRAM — internal RAM
    canvas.setColorDepth(8);  // 8-bit color frees ~32 KB for the echo tape
    if (!canvas.createSprite(240, 135)) {
      Serial.println("grajek: canvas allocation FAILED");
      displayOk = false;
    }
  }

  // Synth at 48 kHz; the ear runs its listening windows at 16 kHz on the
  // factory M5.Mic path (the full-duplex experiments are over — one bus,
  // one master; see audio_out.h).
  engine.init(48000.0f);
  engine.setParam(ga::Param::FilterCutoffHz, 7500.0f);

  audioOk = hal::audioInit(&engine);
  if (!audioOk) Serial.println("grajek: audio init FAILED");

  // the ambient brain: background chord, weather, ghost garden — alive from
  // boot, in the menu too (the box hums the moment it wakes up)
  ambient::init(&engine);
  pulse::init(&engine);  // the heart waits for a few even key presses

  // zapamiętane wybory (skala, barwa, oktawa, tło) wracają po włączeniu
  settings::load();
  settings::applyToEngine(engine);
  if (!settings::backgroundOn()) ambient::backgroundSetEnabled(false);
  // ...i dusza: ogród wspomnień, własne tło, puls — z jednym cichym
  // powitaniem wczorajszą nutą (patrz soul.h)
  soul::load();

  input::keysInit();

  // pudełko budzi się GRAJĄCE — ustawienia mieszkają pod GO
  ModeCtx bootCtx{engine, canvas};
  switchTo(bootCtx, playMode());

  lastTickMs = millis();
}

void loop() {
  // dt measured at the top of the pass so draw/pushSprite time is included
  const uint32_t now = millis();
  const float dt = (float)(now - lastTickMs) / 1000.0f;
  lastTickMs = now;

  // diagnostyka po serialu: pojedyncze znaki sterują kodekiem (patrz
  // hal::audioDiag), a linia "Wrrvv" (hex) pisze dowolny rejestr ES8311 —
  // pozwala bisektować konfigurację bez flashowania
  {
    static char line[8];
    static int len = 0;
    while (Serial.available() > 0) {
      const char c = (char)Serial.read();
      if (c == '\n' || c == '\r') {
        if (len == 1) {
          hal::audioDiag(line[0]);
        } else if (len == 5 && (line[0] == 'W' || line[0] == 'w')) {
          const long rv = strtol(line + 1, nullptr, 16);  // "rrvv" hex
          hal::audioDiagWrite((uint8_t)((rv >> 8) & 0xFF), (uint8_t)(rv & 0xFF));
        }
        len = 0;
      } else if (len < 7) {
        line[len++] = c;
        line[len] = 0;
      }
    }
  }
  // telemetria ucha przy diagnostyce 'e' (poza trybem ŚPIEW, który loguje sam)
  static uint32_t lastDiagMs = 0;
  if (!settings::singOn() && hal::earIsOpen() && now - lastDiagMs > 1000) {
    lastDiagMs = now;
    Serial.printf("grajek: poziom=%.4f\n", hal::earLevel());
  }

  // UWAGA: bez M5Cardputer.update() — klawiaturę czytamy sami w keysPoll
  // (polling FIFO TCA8418; czytnik biblioteki na przerwaniu potrafił się
  // zakleszczyć i zamrażał klawiaturę), a update() podkradałby zdarzenia
  input::KeyEvent ev[input::kMaxKeyEvents];
  const int n = input::keysPoll(ev, input::kMaxKeyEvents);
  ModeCtx ctx{engine, canvas};

  // GO: przytrzymanie PODCZAS GRANIA = następna barwa (powtarzane),
  // krótkie puszczenie = przeskok granie <-> ustawienia
  bool justSwitched = false;
  if (!inSettings && input::goHeldMs() >= goNextActionMs) {
    current->onGoHold(ctx);
    goNextActionMs += 700;
    goLongUsed = true;
  }
  if (input::goReleased()) {
    if (!goLongUsed) {
      inSettings = !inSettings;
      switchTo(ctx, inSettings ? (Mode*)&modeSettings : playMode());
      justSwitched = true;  // klawisz-cyfra z tego przebiegu nie zagra nuty
    }
    goLongUsed = false;
    goNextActionMs = 600;
  }

  if (!justSwitched)
    for (int i = 0; i < n; ++i)
      current->onKey(ctx, ev[i].col, ev[i].row, ev[i].down);
  current->tick(ctx, dt);

  if (displayOk && current->dirty() && now - lastFrameMs >= 40) {  // ~25 fps
    current->draw(ctx);
    canvas.pushSprite(&M5Cardputer.Display, 0, 0);
    current->clearDirty();
    lastFrameMs = now;
  }

  ambient::tick();
  pulse::tick();

  delay(2);  // breathing room for WDT/USB; audio lives on the other core anyway
}
