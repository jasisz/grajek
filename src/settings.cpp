#include "settings.h"

#include <Preferences.h>

#include "ambient.h"
#include "hal/audio_out.h"
#include "i18n.h"
#include "viz.h"

namespace {

const float kBaseOctaves[4] = {55.0f, 110.0f, 220.0f, 440.0f};
const float kVolumeGain[3] = {0.35f, 0.6f, 0.9f};
gk::Worlds s_worlds;
settings::OutputMode s_output = settings::OutputMode::Speaker;
settings::Volume s_volume = settings::Volume::Medium;

constexpr uint8_t kDirtyWorlds = 1u << 0;
constexpr uint8_t kDirtyOutput = 1u << 1;
constexpr uint8_t kDirtyVolume = 1u << 2;
uint8_t s_dirty = 0;
bool s_forceBackground = false;

void changed(uint8_t field = kDirtyWorlds) {
  s_dirty |= field;
  ambient::settingsChanged();
}

gk::WorldSettings loadLegacy(Preferences& p) {
  gk::WorldSettings legacy{};
  const int scaleIdx = p.getUChar("skala", 0) % (int)ga::scalePluginCount();
  legacy.scale = static_cast<uint8_t>(ga::scalePluginAt(scaleIdx).id);
  legacy.preset = p.getUChar("barwa", 3) % ga::kNumTimbrePresets;
  legacy.octave = p.getUChar("okt", 2) % 4;
  const bool hasBackground = p.isKey("bgmode");
  legacy.background = gk::decodeBackgroundSetting(
      hasBackground, p.getUChar("bgmode", (uint8_t)gk::BackgroundId::Drone),
      !hasBackground && p.isKey("tlo"), p.getBool("tlo", true)).id;
  legacy.scene = p.getUChar("scena", 0) % viz::kSceneCount;
  const uint8_t glide = p.getUChar("glidem", p.getBool("glide", false) ? 1 : 0);
  legacy.glide = glide > 2 ? 0 : glide;
  return legacy;
}

}  // namespace

namespace settings {

void load() {
  s_worlds = gk::Worlds{};
  s_output = OutputMode::Speaker;
  s_volume = Volume::Medium;
  s_dirty = 0;
  s_forceBackground = false;
  Preferences p;
  if (!p.begin("grajek", true)) return;  // fresh device: meadow defaults
  gk::WorldSnapshot disk{};
  const size_t size = p.getBytesLength("worlds");
  const bool restored = (size == sizeof(disk) || size == sizeof(gk::WorldSnapshotV1)) &&
                        p.getBytes("worlds", &disk, sizeof(disk)) == size &&
                        s_worlds.restore(&disk, size);
  if (restored && size != sizeof(disk)) s_dirty |= kDirtyWorlds;
  if (!restored) {
    // Import manual choices without replacing them with a world's defaults.
    // Keep the old keys readable for firmware downgrades; new saves use one blob.
    s_worlds.migrate(loadLegacy(p));
    s_dirty |= kDirtyWorlds;
  }
  s_output = p.getUChar("out", 0) ? OutputMode::Jack : OutputMode::Speaker;
  const uint8_t vol = p.getUChar("vol", (uint8_t)Volume::Medium);
  s_volume = (Volume)(vol > 2 ? (uint8_t)Volume::Medium : vol);
  p.end();
}

bool save() {
  if (!s_dirty) return true;
  const bool parked = hal::audioParkForFlash();
  if (hal::audioReady() && !parked) return false;
  Preferences p;
  if (!p.begin("grajek", false)) {
    if (parked) hal::audioResumeAfterFlash();
    return false;
  }
  uint8_t saved = 0;
  if (s_dirty & kDirtyWorlds) {
    const auto disk = s_worlds.snapshot();
    if (p.putBytes("worlds", &disk, sizeof(disk)) == sizeof(disk))
      saved |= kDirtyWorlds;
  }
  if ((s_dirty & kDirtyOutput) &&
      p.putUChar("out", (uint8_t)s_output) == sizeof(uint8_t))
    saved |= kDirtyOutput;
  if ((s_dirty & kDirtyVolume) &&
      p.putUChar("vol", (uint8_t)s_volume) == sizeof(uint8_t))
    saved |= kDirtyVolume;
  p.end();
  if (parked) hal::audioResumeAfterFlash();
  s_dirty &= ~saved;
  if (s_dirty)
    Serial.println("grajek: czesc ustawien nie zapisala sie — sprobuje pozniej");
  return s_dirty == 0;
}

gk::WorldId world() { return s_worlds.selected(); }
const gk::WorldSound& worldSound() { return gk::worldSound(world()); }

void cycleWorld() {
  s_worlds.next();
  // A deliberate world change adopts its background, including when a legacy
  // custom chord had the same built-in preset ID. The shared garden survives.
  s_forceBackground = true;
  changed();
}

ga::ScaleId scale() { return (ga::ScaleId)s_worlds.current().scale; }
int preset() { return s_worlds.current().preset; }
int octave() { return s_worlds.current().octave; }
float baseHz() { return kBaseOctaves[octave()]; }
gk::BackgroundId backgroundPreset() { return s_worlds.current().background; }
int vizScene() { return s_worlds.current().scene; }
GlideMode glide() { return (GlideMode)s_worlds.current().glide; }
OutputMode output() { return s_output; }
Volume volume() { return s_volume; }

const char* presetName(int idx) { return i18n::presetName(idx); }

void cycleScale() {
  const size_t next = (ga::scalePluginIndex(scale()) + 1) % ga::scalePluginCount();
  s_worlds.current().scale = (uint8_t)ga::scalePluginAt(next).id;
  changed();
}

void cyclePreset() {
  auto& value = s_worlds.current().preset;
  value = (value + 1) % ga::kNumTimbrePresets;
  changed();
}

void cycleOctave() {
  auto& value = s_worlds.current().octave;
  value = (value + 1) % 4;
  changed();
}

void cycleBackground() {
  auto& value = s_worlds.current().background;
  value = (gk::BackgroundId)(((int)value + 1) % gk::backgroundPresetCount());
  ambient::backgroundSetPreset(value);
  changed();
}

void cycleVizScene() {
  auto& value = s_worlds.current().scene;
  value = (value + 1) % viz::kSceneCount;
  viz::setScene(value);
  changed();
}

void cycleGlide() {
  auto& value = s_worlds.current().glide;
  value = (value + 1) % 3;
  changed();
}

void cycleOutput() {
  s_output = s_output == OutputMode::Speaker ? OutputMode::Jack : OutputMode::Speaker;
  changed(kDirtyOutput);
}

void cycleVolume() {
  s_volume = (Volume)(((uint8_t)s_volume + 1) % 3);
  changed(kDirtyVolume);
}

void applyToEngine(ga::Engine& e) {
  const float root = baseHz();
  e.setParam(ga::Param::BaseHz, root);
  hal::strings().setRootHz(root);
  e.setParam(ga::Param::TimbrePreset, (float)preset());
  hal::chorus().setDepth(ga::timbrePreset(preset()).chorusDepth);
  const bool jack = s_output == OutputMode::Jack;
  hal::setJackVoicing(jack);
  e.setParam(ga::Param::BassVoicing, jack ? 0.0f : 1.0f);
  e.setParam(ga::Param::MasterGain, kVolumeGain[(uint8_t)s_volume]);
  ambient::setPreset(preset());
  ambient::setWorld(world());
  if (s_forceBackground || ambient::backgroundSelectedPreset() != backgroundPreset()) {
    ambient::backgroundSetPreset(backgroundPreset());
    s_forceBackground = false;
  }
}

}  // namespace settings
