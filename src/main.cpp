// grajek — an experimental instrument on the M5Stack Cardputer-ADV.
//
// Core split:
//   core 0: audio task (hal/audio_out.cpp) — synthesis + I2S, nothing blocks it
//   core 1: Arduino loop — keyboard, IMU, LCD, mode logic
// The two sides talk exclusively through the engine's lock-free event queue.
#include <M5Cardputer.h>

#include "ambient.h"
#include "firefly.h"
#include "ga_engine.h"
#include "goodnight.h"
#include "hal/audio_out.h"
#include "input/keys.h"
#include "modes/mode.h"
#include "modes/mode_instrument.h"
#include "modes/mode_settings.h"
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
// zmienia barwę. Granie to zawsze INSTRUMENT.
ModeInstrument modeInstrument;
ModeSettings modeSettings;

Mode* current = nullptr;
bool inSettings = false;
bool audioOk = false;
bool displayOk = false;  // M5GFX autodetect can fail — never draw blind:
                         // pushSprite on a panel-less display hard-crashes
bool displaySleeping = false;
uint32_t lastFrameMs = 0;
uint32_t lastTickMs = 0;

// GO: krótkie puszczenie = powrót do menu, przytrzymanie = akcja trybu
// (w INSTRUMENT: następna barwa), powtarzana co 700 ms póki trzymany
bool goLongUsed = false;
uint32_t goNextActionMs = 600;

void switchTo(ModeCtx& ctx, Mode* next) {
  const bool refreshBackground = current != nullptr;
  if (current) {
    engine.allNotesOff();
    current->exit(ctx);
  }
  current = next;
  Serial.printf("grajek: -> %s\n", next->name());
  next->enter(ctx);
  // ModeSettings::enter may write NVS. Keep the gap between all-notes-off
  // and this refresh truly quiet so flash-cache stalls cannot scratch a live
  // drone. During goodnight backgroundRefresh deliberately remains a no-op.
  if (refreshBackground) ambient::backgroundRefresh();
}

}  // namespace

void setup() {
  Serial.begin(115200);
  auto cfg = M5.config();
  cfg.internal_spk = false;  // we drive the codec ourselves (low latency)
  cfg.internal_mic = false;  // this instrument never listens
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

  // Synth at 48 kHz.
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
  // ...i dusza: ogród wspomnień, własne tło, puls — z jednym cichym
  // powitaniem wczorajszą nutą (patrz soul.h)
  soul::load();

  input::keysInit();

  // pudełko budzi się GRAJĄCE — ustawienia mieszkają pod GO
  firefly::hello();  // jeden ciepły błysk: żyję (i tu mieszka świetlik)
  ModeCtx bootCtx{engine, canvas};
  switchTo(bootCtx, &modeInstrument);

  lastTickMs = millis();
}

void loop() {
  // dt measured at the top of the pass so draw/pushSprite time is included
  const uint32_t now = millis();
  const float dt = (float)(now - lastTickMs) / 1000.0f;
  lastTickMs = now;

  // UWAGA: bez M5Cardputer.update() — klawiaturę czytamy sami w keysPoll
  // (polling FIFO TCA8418; czytnik biblioteki na przerwaniu potrafił się
  // zakleszczyć i zamrażał klawiaturę), a update() podkradałby zdarzenia
  input::KeyEvent ev[input::kMaxKeyEvents];
  const int n = input::keysPoll(ev, input::kMaxKeyEvents);
  ModeCtx ctx{engine, canvas};

  // Physical state is global even when a mode switch intentionally suppresses
  // the musical meaning of events from this pass.
  for (int i = 0; i < n; ++i) {
    ambient::keyState(ev[i].row * 14 + ev[i].col, ev[i].down);
    if (ev[i].down) goodnight::wakeFromKey();
  }
  if (input::goPressed()) goodnight::wakeFromKey();

  // GO: przytrzymanie PODCZAS GRANIA = następna barwa (powtarzane),
  // krótkie puszczenie = przeskok granie <-> ustawienia
  bool justSwitched = false;
  if (!inSettings && input::goHeldMs() >= goNextActionMs) {
    goodnight::wakeFromKey();
    current->onGoHold(ctx);
    goNextActionMs += 700;
    goLongUsed = true;
  }
  if (input::goReleased()) {
    goodnight::wakeFromKey();  // the side button is a physical key too
    if (!goLongUsed) {
      inSettings = !inSettings;
      switchTo(ctx, inSettings ? (Mode*)&modeSettings
                               : (Mode*)&modeInstrument);
      justSwitched = true;  // klawisz-cyfra z tego przebiegu nie zagra nuty
    } else {
      settings::save();  // persist only the final preset reached by the hold
    }
    goLongUsed = false;
    goNextActionMs = 600;
  }

  if (!justSwitched)
    for (int i = 0; i < n; ++i)
      current->onKey(ctx, ev[i].col, ev[i].row, ev[i].down);
  current->tick(ctx, dt);

  // Face-down means bedtime, so put the hidden LCD controller and backlight to
  // sleep as soon as the lullaby begins. The CPU, keyboard and IMU stay awake
  // so lifting the box or touching any key is an instant, reliable alarm clock.
  bool displayJustWoke = false;
  if (displayOk) {
    const bool shouldSleep = ambient::lullabyActive();
    if (shouldSleep && !displaySleeping) {
      firefly::setSleeping(true);
      M5Cardputer.Display.sleep();
      displaySleeping = true;
      Serial.println("grajek: drzemka — ekran zgaszony");
    } else if (!shouldSleep && displaySleeping) {
      M5Cardputer.Display.wakeup();
      firefly::setSleeping(false);
      displaySleeping = false;
      displayJustWoke = true;
      Serial.println("grajek: pobudka — ekran wlaczony");
    }
  }

  if (displayOk && !displaySleeping &&
      (current->dirty() || displayJustWoke) &&
      now - lastFrameMs >= 40) {  // ~25 fps
    current->draw(ctx);
    canvas.pushSprite(&M5Cardputer.Display, 0, 0);
    current->clearDirty();
    lastFrameMs = now;
  }

  const ambient::BeatGrid beatGrid{pulse::nowSec(),
                                   pulse::memoryPeriodSec(),
                                   pulse::lastOnsetSec()};
  ambient::tick(beatGrid);

  // Ambient chooses a musically safe instant; the app coordinator owns the
  // side effect. This removes the ambient <-> persistence dependency cycle.
  const ambient::SaveRequest saveRequest = ambient::saveRequest();
  if (saveRequest != ambient::SaveRequest::None) {
    const bool soulSaved = soul::save();
    bool settingsSaved = true;
    if (saveRequest == ambient::SaveRequest::SoulAndSettings)
      settingsSaved = settings::save();
    ambient::saveFinished(saveRequest, soulSaved && settingsSaved);
  }

  float pulseChord[4];
  const int pulseChordCount = ambient::backgroundNoteCount();
  for (int i = 0; i < pulseChordCount && i < 4; ++i)
    pulseChord[i] = ambient::backgroundNoteCents(i);
  pulse::tick(ambient::lullabyActive(), pulseChord, pulseChordCount,
              settings::preset());
  firefly::tick();

  delay(2);  // breathing room for WDT/USB; audio lives on the other core anyway
}
