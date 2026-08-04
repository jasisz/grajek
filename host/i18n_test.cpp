#include <assert.h>
#include <string.h>

#include "i18n.h"

int main() {
  for (int raw = 0; raw < static_cast<int>(i18n::TextId::Count); ++raw)
    assert(i18n::tr(static_cast<i18n::TextId>(raw))[0] != '\0');
  assert(i18n::tr(i18n::TextId::Count)[0] == '\0');

#if defined(GRAJEK_LANG_EN)
  assert(strcmp(i18n::tr(i18n::TextId::SettingsTitle), "settings") == 0);
  assert(strcmp(i18n::backgroundName(gk::BackgroundId::Off), "off") == 0);
  assert(strcmp(i18n::presetName(4), "MUSICBOX") == 0);
  assert(strcmp(i18n::presetName(5), "WARM") == 0);
  assert(strcmp(i18n::tr(i18n::TextId::GlideSoft), "soft") == 0);
  assert(strcmp(i18n::tr(i18n::TextId::GlideStrong), "strong") == 0);
  assert(strcmp(i18n::tr(i18n::TextId::OutputJack), "jack") == 0);
  assert(strcmp(i18n::scaleName(ga::ScaleId::MAJOR), "MAJOR JI") == 0);
#else
  assert(strcmp(i18n::tr(i18n::TextId::SettingsTitle), "ustawienia") == 0);
  assert(strcmp(i18n::backgroundName(gk::BackgroundId::Off), "cisza") == 0);
  assert(strcmp(i18n::presetName(4), "POZYTYWKA") == 0);
  assert(strcmp(i18n::presetName(5), "AKSAMIT") == 0);
  assert(strcmp(i18n::tr(i18n::TextId::GlideSoft), "lekki") == 0);
  assert(strcmp(i18n::tr(i18n::TextId::GlideStrong), "mocny") == 0);
  assert(strcmp(i18n::tr(i18n::TextId::OutputSpeaker), "glosnik") == 0);
  assert(strcmp(i18n::scaleName(ga::ScaleId::MAJOR), "DUR JI") == 0);
#endif

  assert(strcmp(i18n::scaleName(static_cast<ga::ScaleId>(255)), "12-EDO") ==
         0);
  assert(strcmp(i18n::backgroundName(static_cast<gk::BackgroundId>(99)),
                i18n::backgroundName(gk::BackgroundId::Drone)) == 0);
  assert(strcmp(i18n::presetName(99), i18n::presetName(0)) == 0);
}
