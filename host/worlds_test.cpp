#include <assert.h>
#include <string.h>
#include <stdio.h>

#include "ga_scales.h"
#include "ga_voice.h"
#include "gk_go_button.h"
#include "gk_worlds.h"

namespace {

void navigation() {
  using Action = gk::GoAction;
  gk::GoButton go;
  assert(go.update(1000, true, false, false, false) == Action::None);
  assert(go.update(1120, false, true, false, false) == Action::NextWorld);
  assert(go.update(1121, false, false, false, false) == Action::None);
  assert(go.update(2000, true, false, false, false) == Action::None);
  assert(go.update(2699, false, false, false, false) == Action::None);
  assert(go.update(2700, false, false, false, false) == Action::OpenSettings);
  // The screen changed while GO remains held: no repeat and no second action.
  assert(go.update(5000, false, false, true, false) == Action::None);
  assert(go.update(5010, false, true, true, false) == Action::None);
  assert(go.update(6000, true, false, true, false) == Action::None);
  assert(go.update(6100, false, true, true, false) == Action::Play);
  // A slow frame must still recognize a hold when it first observes release.
  assert(go.update(7000, true, false, false, false) == Action::None);
  assert(go.update(7800, false, true, false, false) == Action::OpenSettings);
  assert(go.update(8000, true, false, true, false) == Action::None);
  assert(go.update(8700, false, false, true, false) == Action::Play);
  assert(go.update(8800, false, true, false, false) == Action::None);

  assert(go.update(10000, true, false, false, true) == Action::Wake);
  assert(go.update(10100, false, true, false, false) == Action::None);
  assert(go.update(11000, true, false, true, true) == Action::Wake);
  assert(go.update(12000, false, false, true, false) == Action::None);
  assert(go.update(13000, false, true, true, false) == Action::None);
  assert(go.update(13100, false, true, false, false) == Action::None);

  const uint32_t start = UINT32_MAX - 400u;
  assert(go.update(start, true, false, false, false) == Action::None);
  assert(go.update(start + 699, false, false, false, false) == Action::None);
  assert(go.update(start + 700, false, false, false, false) == Action::OpenSettings);
  assert(go.update(start + 900, false, true, true, false) == Action::None);
}

void worlds() {
  // World defaults use the existing persisted scale and timbre registries.
  static_assert((uint8_t)ga::ScaleId::PENTA == 4);
  static_assert((uint8_t)ga::ScaleId::Count == 7 && ga::kNumTimbrePresets == 7);
  gk::Worlds bank;
  assert(bank.selected() == gk::WorldId::Meadow);
  assert(bank.current().preset == 4 && bank.current().background == gk::BackgroundId::Off);
  bank.current().scale = (uint8_t)ga::ScaleId::EDO19;
  bank.current().preset = 2;
  bank.current().scene = 4;  // manual scene choices remain independent
  bank.next();
  assert(bank.selected() == gk::WorldId::Ocean);
  assert(bank.current().preset == 6 && bank.current().glide == 2);
  bank.current().octave = 1;
  bank.current().background = gk::BackgroundId::Root;
  bank.next();
  assert(bank.selected() == gk::WorldId::Cosmos);
  assert(bank.current().preset == 3 && bank.current().scene == 1);
  bank.next();
  assert(bank.selected() == gk::WorldId::Fireworks && bank.current().scene == 3);
  bank.current().glide = 2;
  bank.next();
  assert(bank.selected() == gk::WorldId::Mandala && bank.current().scene == 4);
  bank.current().octave = 0;
  bank.next();
  assert(bank.current().preset == 2 && bank.current().scene == 4);
  assert(bank.current().scale == (uint8_t)ga::ScaleId::EDO19);

  bank.next();
  const auto disk = bank.snapshot();
  gk::Worlds restored;
  assert(restored.restore(&disk, sizeof(disk)));
  assert(restored.selected() == gk::WorldId::Ocean);
  assert(restored.current().octave == 1 && restored.current().background == gk::BackgroundId::Root);
  restored.next();
  restored.next();
  assert(restored.selected() == gk::WorldId::Fireworks && restored.current().glide == 2);
  restored.next();
  assert(restored.selected() == gk::WorldId::Mandala && restored.current().octave == 0);
  restored.next();
  assert(restored.current().preset == 2 && restored.current().scene == 4);

  auto bad = disk;
  bad.worlds[2].preset = 255;
  assert(!restored.restore(&bad, sizeof(bad)));
  assert(restored.selected() == gk::WorldId::Meadow && restored.current().preset == 2);
  bad = disk;
  bad.selected = (gk::WorldId)255;
  assert(!restored.restore(&bad, sizeof(bad)));
  assert(!restored.restore(&disk, sizeof(disk) - 1));
  bad = disk;
  bad.version = 3;
  assert(!restored.restore(&bad, sizeof(bad)));

  // The actual 24-byte v1 layout keeps all three edited profiles and selection.
  gk::WorldSnapshotV1 v1{};
  memcpy(v1.magic, "GKWD", 4);
  v1.version = 1;
  for (int i = 0; i < 3; ++i) v1.worlds[i] = disk.worlds[i];
  for (int selected = 0; selected < 3; ++selected) {
    v1.selected = (gk::WorldId)selected;
    assert(restored.restore(&v1, sizeof(v1)));
    const auto expanded = restored.snapshot();
    assert(expanded.version == 2 && expanded.selected == v1.selected);
    assert(memcmp(expanded.worlds, v1.worlds, sizeof(v1.worlds)) == 0);
    for (int i = 3; i < gk::kWorldCount; ++i)
      assert(memcmp(&expanded.worlds[i], &gk::worldDefaults((gk::WorldId)i),
                    sizeof(gk::WorldSettings)) == 0);
  }
  const auto beforeBad = restored.snapshot();
  v1.selected = gk::WorldId::Fireworks;  // never valid in the old format
  assert(!restored.restore(&v1, sizeof(v1)));
  v1.selected = gk::WorldId::Meadow;
  v1.worlds[2].scene = 255;
  assert(!restored.restore(&v1, sizeof(v1)));
  v1.worlds[2] = disk.worlds[2];
  v1.version = 2;
  assert(!restored.restore(&v1, sizeof(v1)));
  const auto afterBad = restored.snapshot();
  assert(memcmp(&beforeBad, &afterBad, sizeof(beforeBad)) == 0);

  // Existing global edits become that world's personal setup on upgrade.
  const gk::WorldSettings legacy{5, 1, 3, gk::BackgroundId::Root, 2, 1};
  restored.migrate(legacy);
  assert(restored.selected() == gk::WorldId::Ocean);
  assert(memcmp(&legacy, &restored.current(), sizeof(legacy)) == 0);
  restored.next();
  assert(restored.current().preset == gk::worldDefaults(gk::WorldId::Cosmos).preset);
  auto invalid = legacy;
  invalid.scale = 255;
  restored.migrate(invalid);
  assert(restored.selected() == gk::WorldId::Cosmos);

  for (int i = 0; i < gk::kWorldCount; ++i) {
    const auto id = (gk::WorldId)i;
    restored.migrate(gk::worldDefaults(id));
    assert(restored.selected() == id);  // every old scene maps to its world
    assert(gk::validWorldSettings(gk::worldDefaults(id)));
    for (int j = 0; j < i; ++j) {
      assert(gk::worldDefaults(id).scene != gk::worldDefaults((gk::WorldId)j).scene);
      assert(gk::worldDefaults(id).preset != gk::worldDefaults((gk::WorldId)j).preset);
    }
    const auto& sound = gk::worldSound(id);
    assert(sound.echoFeedback >= 0 && sound.echoFeedback + 0.15f <= 0.75f);
    assert(sound.cutoffHz >= 300 && sound.cutoffHz <= 12000);
    assert(sound.room >= 0 && sound.room <= 1);
  }
  assert(gk::worldSound(gk::WorldId::Ocean).tiltBendCents > 0);
  assert(gk::worldSound(gk::WorldId::Meadow).reverbLevelScale <
         gk::worldSound(gk::WorldId::Cosmos).reverbLevelScale);
}

}  // namespace

int main() {
  navigation();
  worlds();
  puts("worlds_test: ok");
}
