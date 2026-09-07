#include "soul.h"

#include <Arduino.h>
#include <Preferences.h>
#include <math.h>
#include <string.h>

#include "ambient.h"
#include "hal/audio_out.h"
#include "pulse.h"
#include "gk_soul.h"

namespace {

constexpr int kChordMax = 4;

using SoulDisk = gk::SoulSnapshot;
constexpr uint8_t kCustomChord = gk::kCustomChord;

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
  mix(ambient::gardenPhraseCount());
  for (int i = 0; i < n; ++i) {
    mix((int32_t)lroundf(ambient::gardenCents(i)));
    mix(ambient::gardenDelayMs(i));
    mix(ambient::gardenHoldMs(i));
    mix(ambient::gardenVelocity(i));
    mix(ambient::gardenStartsPhrase(i) ? 1 : 0);
  }
  const bool custom = ambient::backgroundIsCustom();
  mix(custom ? 1 : 0);
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
  if (!p.begin("dusza", true)) {
    // A missing read-only namespace is the normal first boot. The first real
    // phrase changes the signature and creates it on save.
    s_savedSig = signature();
    return;
  }

  SoulDisk disk{};
  int gN = 0;
  int cN = 0;
  const size_t bytes = p.getBytesLength("snapshot");
  uint8_t raw[sizeof(SoulDisk)]{};
  if ((bytes == gk::kSoulV1Bytes || bytes == sizeof(SoulDisk)) &&
      p.getBytes("snapshot", raw, bytes) == bytes &&
      gk::decodeSoul(raw, bytes, disk)) {
    gN = disk.gardenCount;
    float cents[ambient::kGardenCapacity];
    uint16_t delayMs[ambient::kGardenCapacity];
    for (int i = 0; i < gN; ++i) {
      cents[i] = (float)disk.cents[i];
      delayMs[i] = disk.delayMs[i];
    }
    ambient::gardenRestore(cents, delayMs, disk.phraseStartMask, gN,
                            disk.holdMs, disk.velocity);
    cN = disk.chordCount;
    if (disk.flags & kCustomChord) {
      float chord[kChordMax];
      for (int i = 0; i < cN; ++i) chord[i] = (float)disk.chord[i];
      ambient::backgroundRestoreChord(cN > 0 ? chord : nullptr, cN);
    }
    if (disk.pulseMs > 150)
      pulse::restoreMemory((double)disk.pulseMs * 0.001);
  }

  p.end();
  s_savedSig = signature();
  if (gN > 0)
    Serial.printf("grajek: dusza wraca — %d fraz / %d nut%s\n",
                  ambient::gardenPhraseCount(), gN,
                  cN > 0 ? ", wlasne tlo" : "");
  ambient::scheduleGreeting();  // one remembered note, a moment after waking
}

bool save() {
  const uint32_t sig = signature();
  if (sig == s_savedSig) return true;  // nothing new to remember

  // Opening a missing read-write namespace may itself write NVS. Park before
  // begin(), not merely before putBytes(), so even the very first save is
  // silent.
  const bool parked = hal::audioParkForFlash();
  if (hal::audioReady() && !parked)
    return false;  // failed park: retry at a safer main-loop pass
  Preferences p;
  if (!p.begin("dusza", false)) {
    if (parked) hal::audioResumeAfterFlash();
    Serial.println("grajek: dusza: NVS niedostepne przy zapisie");
    return false;
  }

  SoulDisk disk{};
  memcpy(disk.magic, "DSZA", 4);
  disk.version = 2;
  disk.bytes = sizeof(disk);
  const int gN = ambient::gardenCount();
  disk.gardenCount = (uint8_t)gN;
  for (int i = 0; i < gN; ++i) {
    disk.cents[i] = (int16_t)lroundf(ambient::gardenCents(i));
    disk.delayMs[i] = ambient::gardenDelayMs(i);
    disk.holdMs[i] = ambient::gardenHoldMs(i);
    disk.velocity[i] = ambient::gardenVelocity(i);
    if (ambient::gardenStartsPhrase(i)) disk.phraseStartMask |= 1u << i;
  }
  if (ambient::backgroundIsCustom()) {
    const int cN = ambient::backgroundCount();
    disk.flags |= kCustomChord;
    disk.chordCount = (uint8_t)cN;
    for (int i = 0; i < cN && i < kChordMax; ++i)
      disk.chord[i] =
          (int16_t)lroundf(ambient::backgroundNoteCents(i));
  }
  disk.pulseMs =
      (uint16_t)fmin(65535.0, pulse::memoryPeriodSec() * 1000.0);
  const bool saved =
      p.putBytes("snapshot", &disk, sizeof(disk)) == sizeof(disk);
  p.end();
  if (parked) hal::audioResumeAfterFlash();
  if (saved) {
    s_savedSig = sig;
    return true;
  } else {
    Serial.println("grajek: zapis duszy NIE UDAL SIE — sprobuje ponownie");
    return false;
  }
}

}  // namespace soul
