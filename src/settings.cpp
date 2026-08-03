#include "settings.h"

#include <Preferences.h>

#include "ambient.h"
#include "hal/audio_out.h"
#include "i18n.h"
#include "viz.h"

namespace {

const float kBaseOctaves[4] = {55.0f, 110.0f, 220.0f, 440.0f};

int s_scaleIdx = 0;  // indeks w statycznym rejestrze (PENTA)
int s_preset = 3;    // CHIME — ładne samo z siebie
int s_octave = 2;    // 220 Hz
gk::BackgroundId s_bgPreset = gk::BackgroundId::Drone;
int s_vizScene = 0;  // nocna łąka

constexpr uint8_t kDirtyScale = 1u << 0;
constexpr uint8_t kDirtyPreset = 1u << 1;
constexpr uint8_t kDirtyOctave = 1u << 2;
constexpr uint8_t kDirtyBackground = 1u << 3;
constexpr uint8_t kDirtyScene = 1u << 4;
uint8_t s_dirty = 0;

}  // namespace

namespace settings {

void load() {
  Preferences p;
  if (!p.begin("grajek", true)) return;  // normal on a fresh device
  s_scaleIdx = p.getUChar("skala", 0) % (int)ga::scalePluginCount();
  s_preset = p.getUChar("barwa", 3) % ga::kNumTimbrePresets;
  s_octave = p.getUChar("okt", 2) % 4;
  const bool hasBackgroundPreset = p.isKey("bgmode");
  const uint8_t backgroundRaw =
      hasBackgroundPreset
          ? p.getUChar("bgmode",
                       static_cast<uint8_t>(gk::BackgroundId::Drone))
          : static_cast<uint8_t>(gk::BackgroundId::Drone);
  const bool hasLegacyBackground =
      !hasBackgroundPreset && p.isKey("tlo");
  const bool legacyBackgroundOn =
      hasLegacyBackground ? p.getBool("tlo", true) : true;
  const gk::BackgroundSetting background = gk::decodeBackgroundSetting(
      hasBackgroundPreset, backgroundRaw, hasLegacyBackground,
      legacyBackgroundOn);
  s_bgPreset = background.id;
  s_vizScene = p.getUChar("scena", 0) % viz::kSceneCount;
  p.end();
  s_dirty = background.needsWrite ? kDirtyBackground : 0;
}

bool save() {
  if (!s_dirty) return true;
  // A fresh read-write namespace can allocate its NVS entry in begin(). Keep
  // that first flash access inside the same quiet window as the value writes.
  const bool parked = hal::audioParkForFlash();
  if (hal::audioReady() && !parked) return false;
  Preferences p;
  if (!p.begin("grajek", false)) {
    if (parked) hal::audioResumeAfterFlash();
    return false;
  }
  uint8_t saved = 0;
  if ((s_dirty & kDirtyScale) &&
      p.putUChar("skala", (uint8_t)s_scaleIdx) == sizeof(uint8_t))
    saved |= kDirtyScale;
  if ((s_dirty & kDirtyPreset) &&
      p.putUChar("barwa", (uint8_t)s_preset) == sizeof(uint8_t))
    saved |= kDirtyPreset;
  if ((s_dirty & kDirtyOctave) &&
      p.putUChar("okt", (uint8_t)s_octave) == sizeof(uint8_t))
    saved |= kDirtyOctave;
  if ((s_dirty & kDirtyBackground) &&
      p.putUChar("bgmode", static_cast<uint8_t>(s_bgPreset)) ==
          sizeof(uint8_t))
    saved |= kDirtyBackground;
  if ((s_dirty & kDirtyScene) &&
      p.putUChar("scena", (uint8_t)s_vizScene) == sizeof(uint8_t))
    saved |= kDirtyScene;
  p.end();
  if (parked) hal::audioResumeAfterFlash();
  s_dirty &= ~saved;
  if (s_dirty)
    Serial.println("grajek: czesc ustawien nie zapisala sie — sprobuje pozniej");
  return s_dirty == 0;
}

ga::ScaleId scale() { return ga::scalePluginAt(s_scaleIdx).id; }
int preset() { return s_preset; }
int octave() { return s_octave; }
float baseHz() { return kBaseOctaves[s_octave]; }
gk::BackgroundId backgroundPreset() { return s_bgPreset; }

const char* presetName(int idx) {
  if (idx < 0 || idx >= ga::kNumTimbrePresets) idx = 0;
  return i18n::presetName(idx);
}

void cycleScale() {
  s_scaleIdx = (s_scaleIdx + 1) % (int)ga::scalePluginCount();
  s_dirty |= kDirtyScale;
}

void cyclePreset() {
  s_preset = (s_preset + 1) % ga::kNumTimbrePresets;
  s_dirty |= kDirtyPreset;
}

void cycleOctave() {
  s_octave = (s_octave + 1) % 4;
  s_dirty |= kDirtyOctave;
}

void cycleBackground() {
  const int next =
      (static_cast<int>(s_bgPreset) + 1) % gk::backgroundPresetCount();
  s_bgPreset = static_cast<gk::BackgroundId>(next);
  ambient::backgroundSetPreset(s_bgPreset);
  s_dirty |= kDirtyBackground;
}

int vizScene() { return s_vizScene; }

void cycleVizScene() {
  s_vizScene = (s_vizScene + 1) % viz::kSceneCount;
  viz::setScene(s_vizScene);
  s_dirty |= kDirtyScene;
}

void applyToEngine(ga::Engine& e) {
  const float root = baseHz();
  e.setParam(ga::Param::BaseHz, root);
  hal::strings().setRootHz(root);  // halo strun dostraja się do centrum
  e.setParam(ga::Param::TimbrePreset, (float)s_preset);
  ambient::setPreset(s_preset);
  // Preserve a soul-restored custom chord on later timbre/octave changes.
  if (ambient::backgroundSelectedPreset() != s_bgPreset)
    ambient::backgroundSetPreset(s_bgPreset);
}

}  // namespace settings
