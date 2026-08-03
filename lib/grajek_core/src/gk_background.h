// Fixed background-sound presets shared by firmware adapters and tests.
//
// This is a descriptor registry, not a runtime plugin loader: the device has
// no heap cost, file format or discovery step, while adding a new preset is a
// single table entry. Numeric ids are persisted and must never be reordered.
#pragma once

#include <stdint.h>

namespace gk {

constexpr int kBackgroundMaxNotes = 3;

enum class BackgroundId : uint8_t {
  Off = 0,
  Root = 1,
  Drone = 2,
  Halo = 3,
  Count = 4,
};

struct BackgroundPreset {
  BackgroundId id;
  float cents[kBackgroundMaxNotes];
  uint8_t noteCount;
  bool weatherMorph;
};

struct BackgroundSetting {
  BackgroundId id;
  bool needsWrite;  // persist the normalized/new-format value
};

int backgroundPresetCount();
const BackgroundPreset& backgroundPreset(BackgroundId id);
const BackgroundPreset& backgroundPresetAt(int index);

// Decode the current byte setting plus the legacy on/off setting. Callers own
// the storage keys; this pure policy keeps migration behavior host-testable.
BackgroundSetting decodeBackgroundSetting(bool hasPreset, uint8_t presetRaw,
                                          bool hasLegacy, bool legacyEnabled);

}  // namespace gk
