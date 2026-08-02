#include "soul.h"

#include <Arduino.h>
#include <Preferences.h>
#include <math.h>
#include <string.h>

#include "ambient.h"
#include "hal/audio_out.h"
#include "pulse.h"

namespace {

constexpr int kChordMax = 4;

struct SoulDiskV1 {
  char magic[4];
  uint8_t version;
  uint8_t gardenCount;
  uint8_t chordCount;
  uint8_t flags;
  uint16_t bytes;
  uint16_t pulseMs;
  uint32_t phraseStartMask;
  int16_t cents[ambient::kGardenCapacity];
  uint16_t delayMs[ambient::kGardenCapacity];
  int16_t chord[kChordMax];
};
static_assert(sizeof(SoulDiskV1) == 152, "stable atomic NVS soul format");
constexpr uint8_t kCustomChord = 1u;

bool validSoul(const SoulDiskV1& s) {
  if (memcmp(s.magic, "DSZA", 4) != 0 || s.version != 1 ||
      s.bytes != sizeof(SoulDiskV1) ||
      s.gardenCount > ambient::kGardenCapacity || s.chordCount > kChordMax ||
      (s.flags & ~kCustomChord) != 0)
    return false;
  if (!(s.flags & kCustomChord) && s.chordCount != 0) return false;
  const uint32_t used =
      s.gardenCount == 32
          ? UINT32_MAX
          : (s.gardenCount == 0 ? 0 : (1u << s.gardenCount) - 1u);
  if ((s.phraseStartMask & ~used) != 0) return false;
  if (s.gardenCount == 0) return s.phraseStartMask == 0;
  if ((s.phraseStartMask & 1u) == 0) return false;
  for (int i = 0; i < s.gardenCount; ++i)
    if ((s.phraseStartMask & (1u << i)) && s.delayMs[i] != 0) return false;
  return true;
}

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

  SoulDiskV1 disk{};
  int gN = 0;
  int cN = 0;
  if (p.getBytesLength("snapshot") == sizeof(disk) &&
      p.getBytes("snapshot", &disk, sizeof(disk)) == sizeof(disk) &&
      validSoul(disk)) {
    gN = disk.gardenCount;
    float cents[ambient::kGardenCapacity];
    uint16_t delayMs[ambient::kGardenCapacity];
    for (int i = 0; i < gN; ++i) {
      cents[i] = (float)disk.cents[i];
      delayMs[i] = disk.delayMs[i];
    }
    ambient::gardenRestore(cents, delayMs, disk.phraseStartMask, gN);
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

  SoulDiskV1 disk{};
  memcpy(disk.magic, "DSZA", 4);
  disk.version = 1;
  disk.bytes = sizeof(disk);
  const int gN = ambient::gardenCount();
  disk.gardenCount = (uint8_t)gN;
  for (int i = 0; i < gN; ++i) {
    disk.cents[i] = (int16_t)lroundf(ambient::gardenCents(i));
    disk.delayMs[i] = ambient::gardenDelayMs(i);
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
