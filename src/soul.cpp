#include "soul.h"

#include <Arduino.h>
#include <Preferences.h>
#include <math.h>

#include "ambient.h"
#include "pulse.h"

namespace {

constexpr int kGardenMax = 32;
constexpr int kChordMax = 4;

// cheap fingerprint of everything the soul stores — save() becomes a no-op
// when nothing changed, so NVS flash is not worn by every settings visit
uint32_t signature() {
  uint32_t h = 2166136261u;
  auto mix = [&h](int32_t v) {
    h ^= (uint32_t)v;
    h *= 16777619u;
  };
  const int n = ambient::gardenCount();
  mix(n);
  for (int i = 0; i < n; ++i) mix((int32_t)lroundf(ambient::gardenCents(i)));
  const bool custom = ambient::backgroundIsCustom();
  mix(custom ? ambient::backgroundCount() : 0);
  if (custom)
    for (int i = 0; i < ambient::backgroundCount(); ++i)
      mix((int32_t)lroundf(ambient::backgroundNoteCents(i)));
  mix((int32_t)(pulse::memoryPeriodSec() * 1000.0));
  return h;
}

uint32_t s_savedSig = 0;

}  // namespace

namespace soul {

void load() {
  Preferences p;
  p.begin("dusza", true);

  int16_t garden[kGardenMax];
  const int gBytes = (int)p.getBytes("ogrod", garden, sizeof(garden));
  const int gN = gBytes / (int)sizeof(int16_t);
  if (gN > 0) {
    float cents[kGardenMax];
    for (int i = 0; i < gN; ++i) cents[i] = (float)garden[i];
    ambient::gardenRestore(cents, gN);
  }

  int16_t chord[kChordMax];
  const int cBytes = (int)p.getBytes("tlo", chord, sizeof(chord));
  const int cN = cBytes / (int)sizeof(int16_t);
  if (cN > 0) {
    float cents[kChordMax];
    for (int i = 0; i < cN; ++i) cents[i] = (float)chord[i];
    ambient::backgroundRestoreChord(cents, cN);
  }

  const uint16_t pulseMs = p.getUShort("puls", 0);
  if (pulseMs > 150) pulse::restoreMemory((double)pulseMs * 0.001);

  p.end();
  s_savedSig = signature();
  if (gN > 0)
    Serial.printf("grajek: dusza wraca — %d wspomnien%s\n", gN,
                  cN > 0 ? ", wlasne tlo" : "");
  ambient::scheduleGreeting();  // one remembered note, a moment after waking
}

void save() {
  const uint32_t sig = signature();
  if (sig == s_savedSig) return;  // nothing new to remember
  s_savedSig = sig;

  Preferences p;
  p.begin("dusza", false);

  int16_t garden[kGardenMax];
  const int gN = ambient::gardenCount();
  for (int i = 0; i < gN && i < kGardenMax; ++i)
    garden[i] = (int16_t)lroundf(ambient::gardenCents(i));
  p.putBytes("ogrod", garden, (size_t)gN * sizeof(int16_t));

  if (ambient::backgroundIsCustom()) {
    int16_t chord[kChordMax];
    const int cN = ambient::backgroundCount();
    for (int i = 0; i < cN && i < kChordMax; ++i)
      chord[i] = (int16_t)lroundf(ambient::backgroundNoteCents(i));
    p.putBytes("tlo", chord, (size_t)cN * sizeof(int16_t));
  } else {
    p.remove("tlo");  // the default chord is a mood, not a memory
  }

  p.putUShort("puls", (uint16_t)fmin(65535.0, pulse::memoryPeriodSec() * 1000.0));
  p.end();
}

}  // namespace soul
