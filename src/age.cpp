#include "age.h"

#include <Preferences.h>

namespace {
age::Tier s_tier = age::Duzy;  // default: full instrument — the parent
                               // dials it down for the child, once
}

namespace age {

void load() {
  Preferences p;
  p.begin("grajek", true);
  const uint8_t v = p.getUChar("wiek", (uint8_t)Duzy);
  p.end();
  s_tier = v <= (uint8_t)Duzy ? (Tier)v : Duzy;
}

Tier tier() { return s_tier; }

void cycle() {
  s_tier = (Tier)(((int)s_tier + 1) % 3);
  Preferences p;
  p.begin("grajek", false);
  p.putUChar("wiek", (uint8_t)s_tier);
  p.end();
}

const char* label(Tier t) {
  switch (t) {
    case Maluch:   return "2-3";
    case Sredniak: return "4-6";
    default:       return "7+";
  }
}

}  // namespace age
