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
bool s_singOn = false;

void save() {
  Preferences p;
  p.begin("grajek", false);
  p.putUChar("skala", (uint8_t)s_scaleIdx);
  p.putUChar("barwa", (uint8_t)s_preset);
  p.putUChar("okt", (uint8_t)s_octave);
  p.putBool("tlo", s_bgOn);
  p.putUChar("scena", (uint8_t)s_vizScene);
  p.putBool("spiew", s_singOn);
  p.end();
}

}  // namespace

namespace settings {

void load() {
  Preferences p;
  p.begin("grajek", true);
  s_scaleIdx = p.getUChar("skala", 0) % (int)ga::ScaleId::Count;
  s_preset = p.getUChar("barwa", 3) % ga::kNumTimbrePresets;
  s_octave = p.getUChar("okt", 2) % 4;
  s_bgOn = p.getBool("tlo", true);
  s_vizScene = p.getUChar("scena", 0) % viz::kSceneCount;
  s_singOn = p.getBool("spiew", false);
  p.end();
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
  save();
}

void cyclePreset() {
  s_preset = (s_preset + 1) % ga::kNumTimbrePresets;
  save();
}

void cycleOctave() {
  s_octave = (s_octave + 1) % 4;
  save();
}

void toggleBackground() {
  s_bgOn = !s_bgOn;
  ambient::backgroundSetEnabled(s_bgOn);
  save();
}

int vizScene() { return s_vizScene; }
bool singOn() { return s_singOn; }

void cycleVizScene() {
  s_vizScene = (s_vizScene + 1) % viz::kSceneCount;
  viz::setScene(s_vizScene);
  save();
}

void toggleSing() {
  s_singOn = !s_singOn;
  save();
}

void applyToEngine(ga::Engine& e) {
  const float root = baseHz();
  e.setParam(ga::Param::BaseHz, root);
  hal::strings().setRootHz(root);  // halo strun dostraja się do centrum
  e.setParam(ga::Param::TimbrePreset, (float)s_preset);
  ambient::setPreset(s_preset);
}

}  // namespace settings
