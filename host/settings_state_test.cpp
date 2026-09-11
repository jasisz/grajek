#include <assert.h>
#include <stdio.h>
#include "Preferences.h"
#include "settings.h"
#include "hal/audio_out.h"
#include "viz.h"

namespace {
ga::SympatheticStrings testStrings;
ga::Chorus testChorus;
gk::BackgroundId background = gk::BackgroundId::Drone;
bool custom = false;
bool jack = false;
gk::WorldId appliedWorld = gk::WorldId::Meadow;
}  // namespace

namespace hal {
bool audioReady() { return true; }
bool audioParkForFlash() {
  ++fake_nvs::parks;
  return fake_nvs::parked = !fake_nvs::failPark;
}
void audioResumeAfterFlash() { ++fake_nvs::resumes; fake_nvs::parked = false; }
ga::SympatheticStrings& strings() { return testStrings; }
ga::Chorus& chorus() { return testChorus; }
void setJackVoicing(bool value) { jack = value; }
}  // namespace hal
namespace ambient {
void settingsChanged() { ++fake_nvs::saveRequests; }
void setPreset(int) {}
void setWorld(gk::WorldId value) { appliedWorld = value; }
gk::BackgroundId backgroundSelectedPreset() { return background; }
void backgroundSetPreset(gk::BackgroundId value) { background = value; custom = false; }
}  // namespace ambient
namespace viz { void setScene(int) {} }

int main() {
  using namespace fake_nvs;
  settings::load();
  assert(settings::world() == gk::WorldId::Meadow && settings::preset() == 4);
  assert(settings::volume() == settings::Volume::Medium);
  settings::cyclePreset();  // my meadow is WARM
  settings::cycleVolume();
  settings::cycleOutput();
  settings::cycleWorld();
  assert(settings::world() == gk::WorldId::Ocean && settings::preset() == 6);
  assert(settings::volume() == settings::Volume::Loud);
  assert(settings::output() == settings::OutputMode::Jack);
  settings::cyclePreset();  // my ocean is PURE
  assert(saveRequests == 5 && writes == 0);  // clicks schedule, never write flash

  failPark = true;
  assert(!settings::save() && writes == 0 && resumes == 0);
  failPark = false;
  failOpen = true;
  assert(!settings::save() && writes == 0 && !parked);
  failOpen = false;
  failBlob = true;
  assert(!settings::save() && writes == 0 && !parked);
  failBlob = false;
  assert(settings::save() && writes == 3 && !parked);
  assert(resumes == parks - 1);
  const int before = parks;
  assert(settings::save() && parks == before);  // unchanged state stays silent

  settings::cycleWorld();
  settings::load();  // reboot from the saved snapshot, not unsaved RAM
  assert(settings::world() == gk::WorldId::Ocean && settings::preset() == 0);
  assert(settings::volume() == settings::Volume::Loud && settings::output() == settings::OutputMode::Jack);
  settings::cycleWorld();
  settings::cycleWorld();
  settings::cycleWorld();
  settings::cycleWorld();
  assert(settings::world() == gk::WorldId::Meadow && settings::preset() == 5);
  ga::Engine engine;
  engine.init(48000);
  settings::applyToEngine(engine);
  assert(jack && appliedWorld == gk::WorldId::Meadow);
  assert(background == gk::BackgroundId::Off);

  // Legacy byte settings migrate without resetting the chosen scale/timbre.
  values = {{"skala", {1}}, {"barwa", {2}}, {"okt", {3}}, {"tlo", {0}},
            {"scena", {2}}, {"glide", {1}}, {"out", {1}}, {"vol", {0}}};
  settings::load();
  assert(settings::world() == gk::WorldId::Ocean);
  assert(settings::scale() == ga::ScaleId::MAJOR && settings::preset() == 2);
  assert(settings::octave() == 3 && settings::backgroundPreset() == gk::BackgroundId::Off);
  assert(settings::glide() == settings::GlideMode::Soft);
  assert(settings::volume() == settings::Volume::Quiet && settings::output() == settings::OutputMode::Jack);
  assert(settings::save() && values["worlds"].size() == sizeof(gk::WorldSnapshot));
  settings::cycleWorld();
  custom = true;
  background = settings::backgroundPreset();
  settings::applyToEngine(engine);
  assert(!custom && appliedWorld == gk::WorldId::Cosmos);

  // Reboot an already-installed three-world firmware: read the smaller blob,
  // retain edits/selection/global settings, then persist the expanded bank.
  gk::WorldSnapshotV1 old{};
  memcpy(old.magic, "GKWD", 4);
  old.version = 1;
  old.selected = gk::WorldId::Ocean;
  for (int i = 0; i < 3; ++i) old.worlds[i] = gk::worldDefaults((gk::WorldId)i);
  old.worlds[0].preset = 2;
  old.worlds[1].octave = 3;
  const auto* bytes = reinterpret_cast<const uint8_t*>(&old);
  values["worlds"] = std::vector<uint8_t>(bytes, bytes + sizeof(old));
  settings::load();
  assert(settings::world() == gk::WorldId::Ocean && settings::octave() == 3);
  assert(settings::volume() == settings::Volume::Quiet && settings::output() == settings::OutputMode::Jack);
  assert(settings::save() && values["worlds"].size() == sizeof(gk::WorldSnapshot));
  settings::load();
  assert(settings::world() == gk::WorldId::Ocean && settings::octave() == 3);
  settings::cycleWorld();
  settings::cycleWorld();
  assert(settings::world() == gk::WorldId::Fireworks && settings::preset() == 5);
  settings::cyclePreset();
  settings::cycleWorld();
  assert(settings::world() == gk::WorldId::Mandala && settings::preset() == 1);
  settings::cycleOctave();
  assert(settings::save());
  settings::load();
  assert(settings::world() == gk::WorldId::Mandala && settings::octave() == 3);
  settings::cycleWorld();
  assert(settings::world() == gk::WorldId::Meadow && settings::preset() == 2);
  settings::cycleWorld();
  settings::cycleWorld();
  settings::cycleWorld();
  assert(settings::world() == gk::WorldId::Fireworks && settings::preset() == 6);
  puts("settings_state_test: ok");
}
