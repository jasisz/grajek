// Compile-time-only LCD translations. Polish is the default firmware;
// define GRAJEK_LANG_EN to build the English variant. Keeping the choice at
// compile time means there is no language state, NVS key or runtime branch.
#pragma once

#include <stdint.h>

#include "ga_scales.h"
#include "gk_background.h"

namespace i18n {

enum class TextId : uint8_t {
  SettingsTitle,
  GoPlay,
  LabelScale,
  LabelTimbre,
  LabelOctave,
  LabelBackground,
  LabelScene,
  PlayGreeting,

  BackgroundOff,
  BackgroundRoot,
  BackgroundDrone,
  BackgroundHalo,

  PresetPure,
  PresetDrone,
  PresetReed,
  PresetChime,
  PresetMusicBox,

  SceneMeadow,
  SceneCosmos,
  SceneOcean,
  SceneFireworks,
  SceneMandala,

  ScaleEdo12,
  ScaleEdo19,
  ScaleEdo31,
  ScaleJi11,
  ScalePentaJi,
  ScaleMajorJi,
  ScaleWolf,

  Count,
};

// Returned strings have static storage and use ASCII only: the built-in LCD
// font and the toast width calculation are byte-oriented.
const char* tr(TextId id);

// Typed/indexed adapters for places where the selected item is data rather
// than a literal TextId. Invalid values use each registry's safe fallback.
const char* backgroundName(gk::BackgroundId id);
const char* presetName(int idx);      // ga::kNumTimbrePresets order
const char* scaleName(ga::ScaleId id);

}  // namespace i18n
