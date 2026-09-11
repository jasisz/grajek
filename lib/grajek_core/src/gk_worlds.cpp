#include "gk_worlds.h"
#include <string.h>

namespace gk {
namespace {
constexpr WorldSettings kDefaults[] = {
    // pentatonic JI throughout; scene order remains the existing visual ABI.
    {4, 4, 2, BackgroundId::Off,   0, 0},  // meadow: plucked tines, open air
    {4, 6, 2, BackgroundId::Drone, 2, 2},  // ocean: breathing pulse and long glide
    {4, 3, 2, BackgroundId::Halo,  1, 1},  // cosmos: bells over a resonant halo
    {4, 5, 2, BackgroundId::Off,   3, 0},  // fireworks: warm, springy bursts
    {4, 1, 2, BackgroundId::Root,  4, 1},  // mandala: slow organ over one root
};
constexpr WorldSound kSound[] = {
    {7500.0f, 0.28f, 0.18f, 0.30f, 0.35f, 0.50f,  0.0f},
    {4800.0f, 0.75f, 0.48f, 1.00f, 0.66f, 0.45f, 80.0f},
    {8200.0f, 1.15f, 0.60f, 1.50f, 0.80f, 0.18f,  0.0f},
    {9200.0f, 0.45f, 0.25f, 0.50f, 0.45f, 0.35f,  0.0f},
    {6200.0f, 0.55f, 0.40f, 1.15f, 0.70f, 0.60f,  0.0f},
};
static_assert(sizeof(kDefaults) / sizeof(kDefaults[0]) == kWorldCount);
static_assert(sizeof(kSound) / sizeof(kSound[0]) == kWorldCount);
int index(WorldId id) {
  return static_cast<int>(id) < kWorldCount ? static_cast<int>(id) : 0;
}
}  // namespace

const WorldSettings& worldDefaults(WorldId id) { return kDefaults[index(id)]; }
const WorldSound& worldSound(WorldId id) { return kSound[index(id)]; }

bool validWorldSettings(const WorldSettings& s) {
  return s.scale < 7 && s.preset < 7 && s.octave < 4 &&
         static_cast<int>(s.background) < backgroundPresetCount() &&
         s.scene < 5 && s.glide < 3;
}

Worlds::Worlds() {
  for (int i = 0; i < kWorldCount; ++i) worlds_[i] = kDefaults[i];
}

void Worlds::next() {
  selected_ = static_cast<WorldId>((static_cast<int>(selected_) + 1) % kWorldCount);
}

void Worlds::migrate(const WorldSettings& legacy) {
  if (!validWorldSettings(legacy)) return;
  *this = Worlds{};
  for (int i = 0; i < kWorldCount; ++i)
    if (kDefaults[i].scene == legacy.scene) selected_ = static_cast<WorldId>(i);
  current() = legacy;
}

WorldSnapshot Worlds::snapshot() const {
  WorldSnapshot disk{};
  memcpy(disk.magic, "GKWD", 4);
  disk.version = 2;
  disk.selected = selected_;
  for (int i = 0; i < kWorldCount; ++i) disk.worlds[i] = worlds_[i];
  return disk;
}

bool Worlds::restore(const void* bytes, size_t size) {
  if (!bytes) return false;
  // Expand the installed three-world bank without losing any manual edits.
  // Validate the entire input before replacing live state.
  Worlds restored;
  auto disk = restored.snapshot();
  int count;
  uint8_t version;
  if (size == sizeof(WorldSnapshotV1)) {
    count = 3;
    version = 1;
  } else if (size == sizeof(WorldSnapshot)) {
    count = kWorldCount;
    version = 2;
  } else {
    return false;
  }
  memcpy(&disk, bytes, size);
  if (memcmp(disk.magic, "GKWD", 4) != 0 || disk.version != version ||
      static_cast<int>(disk.selected) >= count) return false;
  for (int i = 0; i < count; ++i) {
    if (!validWorldSettings(disk.worlds[i])) return false;
    restored.worlds_[i] = disk.worlds[i];
  }
  restored.selected_ = disk.selected;
  *this = restored;
  return true;
}

}  // namespace gk
