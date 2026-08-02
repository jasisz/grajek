#include "settings.h"

#include <Preferences.h>

#include "ambient.h"
#include "hal/audio_out.h"
#include "viz.h"

namespace {

const char* kPresetNames[ga::kNumTimbrePresets] = {"PURE", "DRONE", "REED",
                                                   "CHIME", "MUSICBOX"};
const float kBaseOctaves[4] = {55.0f, 110.0f, 220.0f, 440.0f};

// kolejność odkrywania: od skal, w których nic nie zabrzmi źle, do coraz
// dziwniejszych — maluch zatrzyma się na początku, starszak dojdzie do
// końca, a na samym końcu czeka WILK, w którym nic nie brzmi czysto
const ga::ScaleId kScaleOrder[(int)ga::ScaleId::Count] = {
    ga::ScaleId::PENTA, ga::ScaleId::MAJOR, ga::ScaleId::EDO12,
    ga::ScaleId::EDO19, ga::ScaleId::EDO31, ga::ScaleId::JI11,
    ga::ScaleId::WOLF,
};

int s_scaleIdx = 0;  // indeks w kScaleOrder (PENTA)
int s_preset = 3;    // CHIME — ładne samo z siebie
int s_octave = 2;    // 220 Hz
bool s_bgOn = true;
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
  s_scaleIdx = p.getUChar("skala", 0) % (int)ga::ScaleId::Count;
  s_preset = p.getUChar("barwa", 3) % ga::kNumTimbrePresets;
  s_octave = p.getUChar("okt", 2) % 4;
  s_bgOn = p.getBool("tlo", true);
  s_vizScene = p.getUChar("scena", 0) % viz::kSceneCount;
  p.end();
  s_dirty = 0;
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
      p.putBool("tlo", s_bgOn) == sizeof(bool))
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

ga::ScaleId scale() { return kScaleOrder[s_scaleIdx]; }
int preset() { return s_preset; }
int octave() { return s_octave; }
float baseHz() { return kBaseOctaves[s_octave]; }
bool backgroundOn() { return s_bgOn; }

const char* presetName(int idx) {
  if (idx < 0 || idx >= ga::kNumTimbrePresets) idx = 0;
  return kPresetNames[idx];
}

void cycleScale() {
  s_scaleIdx = (s_scaleIdx + 1) % (int)ga::ScaleId::Count;
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

void toggleBackground() {
  s_bgOn = !s_bgOn;
  ambient::backgroundSetEnabled(s_bgOn);
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
}

}  // namespace settings
