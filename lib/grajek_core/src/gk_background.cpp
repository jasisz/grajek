#include "gk_background.h"

#include <stddef.h>

namespace {

constexpr gk::BackgroundPreset kPresets[] = {
    {gk::BackgroundId::Off, {0.0f, 0.0f, 0.0f}, 0, false},
    {gk::BackgroundId::Root, {0.0f, 0.0f, 0.0f}, 1, false},
    {gk::BackgroundId::Drone, {0.0f, 702.0f, 0.0f}, 2, true},
    // 1/1 + 3/2 + 7/4: consonant enough to remain a floor, but more alive
    // than the plain fifth drone.
    {gk::BackgroundId::Halo, {0.0f, 702.0f, 968.8f}, 3, false},
};

constexpr int kPresetCount = static_cast<int>(sizeof(kPresets) /
                                               sizeof(kPresets[0]));
static_assert(kPresetCount == static_cast<int>(gk::BackgroundId::Count),
              "each persisted background id needs one descriptor");
static_assert(static_cast<int>(gk::BackgroundId::Off) == 0 &&
                  static_cast<int>(gk::BackgroundId::Root) == 1 &&
                  static_cast<int>(gk::BackgroundId::Drone) == 2 &&
                  static_cast<int>(gk::BackgroundId::Halo) == 3,
              "background ids are persistent ABI");
static_assert(kPresets[0].id == gk::BackgroundId::Off &&
                  kPresets[1].id == gk::BackgroundId::Root &&
                  kPresets[2].id == gk::BackgroundId::Drone &&
                  kPresets[3].id == gk::BackgroundId::Halo,
              "background registry order must match persisted ids");

}  // namespace

namespace gk {

int backgroundPresetCount() { return kPresetCount; }

const BackgroundPreset& backgroundPreset(BackgroundId id) {
  const int index = static_cast<int>(id);
  // Invalid persistent data falls back to today's safe/default drone.
  return index >= 0 && index < kPresetCount
             ? kPresets[index]
             : kPresets[static_cast<int>(BackgroundId::Drone)];
}

const BackgroundPreset& backgroundPresetAt(int index) {
  return index >= 0 && index < kPresetCount
             ? kPresets[index]
             : kPresets[static_cast<int>(BackgroundId::Drone)];
}

BackgroundSetting decodeBackgroundSetting(bool hasPreset, uint8_t presetRaw,
                                          bool hasLegacy,
                                          bool legacyEnabled) {
  if (hasPreset) {
    if (presetRaw < static_cast<uint8_t>(BackgroundId::Count))
      return {static_cast<BackgroundId>(presetRaw), false};
    return {BackgroundId::Drone, true};
  }
  if (hasLegacy)
    return {legacyEnabled ? BackgroundId::Drone : BackgroundId::Off, true};
  return {BackgroundId::Drone, false};
}

}  // namespace gk
