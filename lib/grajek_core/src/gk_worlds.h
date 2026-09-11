// Five musical starting points, with independent user edits. Byte IDs match
// the existing scale, timbre, scene and glide registries (checked on host).
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "gk_background.h"

namespace gk {

// Persisted IDs: append worlds without reordering the existing bank.
enum class WorldId : uint8_t {
  Meadow = 0, Ocean = 1, Cosmos = 2, Fireworks = 3, Mandala = 4, Count = 5
};
constexpr int kWorldCount = static_cast<int>(WorldId::Count);

struct WorldSettings {
  uint8_t scale;
  uint8_t preset;
  uint8_t octave;
  BackgroundId background;
  uint8_t scene;
  uint8_t glide;
};

struct WorldSound {
  float cutoffHz;
  float echoLevelScale;
  float echoFeedback;
  float reverbLevelScale;
  float room;
  float damp;
  float tiltBendCents;
};

const WorldSettings& worldDefaults(WorldId id);
const WorldSound& worldSound(WorldId id);
bool validWorldSettings(const WorldSettings& settings);

struct WorldSnapshot {
  char magic[4];
  uint8_t version;
  WorldId selected;
  WorldSettings worlds[kWorldCount];
};
struct WorldSnapshotV1 {
  char magic[4];
  uint8_t version;
  WorldId selected;
  WorldSettings worlds[3];
};
static_assert(sizeof(WorldSettings) == 6 && sizeof(WorldSnapshot) == 36 &&
              sizeof(WorldSnapshotV1) == 24,
              "world settings have a fixed NVS representation");

class Worlds {
 public:
  Worlds();
  WorldId selected() const { return selected_; }
  WorldSettings& current() { return worlds_[static_cast<int>(selected_)]; }
  const WorldSettings& current() const { return worlds_[static_cast<int>(selected_)]; }
  void next();
  // Import old global settings into the closest visual world, once. Other
  // worlds start with their defaults; current manual choices survive upgrade.
  void migrate(const WorldSettings& legacy);
  WorldSnapshot snapshot() const;
  bool restore(const void* bytes, size_t size);

 private:
  WorldId selected_ = WorldId::Meadow;
  WorldSettings worlds_[kWorldCount];
};

}  // namespace gk
