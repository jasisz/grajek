#include "i18n.h"

#include <stddef.h>

#include "ga_voice.h"

namespace {

using i18n::TextId;

#if defined(GRAJEK_LANG_EN)
constexpr const char* kText[] = {
    "settings",    // SettingsTitle
    "GO = play",   // GoPlay
    "1 scale",     // LabelScale
    "2 sound",     // LabelTimbre
    "3 oct.",      // LabelOctave
    "4 bg",        // LabelBackground
    "5 scene",     // LabelScene
    "play!",       // PlayGreeting

    "off",         // BackgroundOff
    "root",        // BackgroundRoot
    "drone",       // BackgroundDrone
    "halo",        // BackgroundHalo

    "PURE",        // PresetPure
    "DRONE",       // PresetDrone
    "REED",        // PresetReed
    "CHIME",       // PresetChime
    "MUSICBOX",    // PresetMusicBox

    "meadow",      // SceneMeadow
    "cosmos",      // SceneCosmos
    "ocean",       // SceneOcean
    "fireworks",   // SceneFireworks
    "mandala",     // SceneMandala

    "12-EDO",      // ScaleEdo12
    "19-EDO",      // ScaleEdo19
    "31-EDO",      // ScaleEdo31
    "JI 11-limit", // ScaleJi11
    "PENTA JI",    // ScalePentaJi
    "MAJOR JI",    // ScaleMajorJi
    "WOLF",        // ScaleWolf
};
#else
constexpr const char* kText[] = {
    "ustawienia",  // SettingsTitle
    "GO = graj",   // GoPlay
    "1 skala",     // LabelScale
    "2 barwa",     // LabelTimbre
    "3 okt.",      // LabelOctave
    "4 tlo",       // LabelBackground
    "5 scena",     // LabelScene
    "graj!",       // PlayGreeting

    "cisza",       // BackgroundOff
    "fundament",   // BackgroundRoot
    "dron",        // BackgroundDrone
    "aureola",     // BackgroundHalo

    "CZYSTA",      // PresetPure
    "DRON",        // PresetDrone
    "STROIK",      // PresetReed
    "DZWONKI",     // PresetChime
    "POZYTYWKA",   // PresetMusicBox

    "laka",        // SceneMeadow
    "kosmos",      // SceneCosmos
    "ocean",       // SceneOcean
    "ognie",       // SceneFireworks
    "mandala",     // SceneMandala

    "12-EDO",      // ScaleEdo12
    "19-EDO",      // ScaleEdo19
    "31-EDO",      // ScaleEdo31
    "JI 11-limit", // ScaleJi11
    "PENTA JI",    // ScalePentaJi
    "DUR JI",      // ScaleMajorJi
    "WILK",        // ScaleWolf
};
#endif

constexpr size_t kTextCount = sizeof(kText) / sizeof(kText[0]);
static_assert(kTextCount == static_cast<size_t>(TextId::Count),
              "each TextId needs one translation");

constexpr TextId kPresetNames[] = {
    TextId::PresetPure,
    TextId::PresetDrone,
    TextId::PresetReed,
    TextId::PresetChime,
    TextId::PresetMusicBox,
};
static_assert(sizeof(kPresetNames) / sizeof(kPresetNames[0]) ==
                  ga::kNumTimbrePresets,
              "preset translations must follow the synthesis preset order");

}  // namespace

namespace i18n {

const char* tr(TextId id) {
  const size_t idx = static_cast<size_t>(id);
  return idx < kTextCount ? kText[idx] : "";
}

const char* backgroundName(gk::BackgroundId id) {
  switch (id) {
    case gk::BackgroundId::Off:   return tr(TextId::BackgroundOff);
    case gk::BackgroundId::Root:  return tr(TextId::BackgroundRoot);
    case gk::BackgroundId::Drone: return tr(TextId::BackgroundDrone);
    case gk::BackgroundId::Halo:  return tr(TextId::BackgroundHalo);
    default:                      return tr(TextId::BackgroundDrone);
  }
}

const char* presetName(int idx) {
  if (idx < 0 || idx >= ga::kNumTimbrePresets) idx = 0;
  return tr(kPresetNames[idx]);
}

const char* scaleName(ga::ScaleId id) {
  switch (id) {
    case ga::ScaleId::EDO12: return tr(TextId::ScaleEdo12);
    case ga::ScaleId::EDO19: return tr(TextId::ScaleEdo19);
    case ga::ScaleId::EDO31: return tr(TextId::ScaleEdo31);
    case ga::ScaleId::JI11:  return tr(TextId::ScaleJi11);
    case ga::ScaleId::PENTA: return tr(TextId::ScalePentaJi);
    case ga::ScaleId::MAJOR: return tr(TextId::ScaleMajorJi);
    case ga::ScaleId::WOLF:  return tr(TextId::ScaleWolf);
    default:                 return tr(TextId::ScaleEdo12);
  }
}

}  // namespace i18n
