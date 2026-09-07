// Versioned device snapshot. V2 extends the exact 152-byte V1 prefix so old
// memories can be upgraded without losing their notes, timing, chord or pulse.
#pragma once

#include <stddef.h>
#include <string.h>

#include "gk_garden.h"

namespace gk {

struct SoulSnapshot {
  char magic[4];
  uint8_t version;
  uint8_t gardenCount;
  uint8_t chordCount;
  uint8_t flags;
  uint16_t bytes;
  uint16_t pulseMs;
  uint32_t phraseStartMask;
  int16_t cents[kGardenCapacity];
  uint16_t delayMs[kGardenCapacity];
  int16_t chord[4];
  uint16_t holdMs[kGardenCapacity];
  uint8_t velocity[kGardenCapacity];
};
constexpr size_t kSoulV1Bytes = 152;
constexpr uint8_t kCustomChord = 1u;
static_assert(offsetof(SoulSnapshot, holdMs) == kSoulV1Bytes, "V1 prefix changed");
static_assert(sizeof(SoulSnapshot) == 248, "V2 snapshot layout changed");

inline bool decodeSoul(const void* data, size_t size, SoulSnapshot& out) {
  if (!data || (size != kSoulV1Bytes && size != sizeof(SoulSnapshot))) return false;
  SoulSnapshot s{};
  memcpy(&s, data, size);
  const bool legacy = size == kSoulV1Bytes;
  if (memcmp(s.magic, "DSZA", 4) != 0 || s.version != (legacy ? 1 : 2) ||
      s.bytes != size || s.gardenCount > kGardenCapacity || s.chordCount > 4 ||
      (s.flags & ~kCustomChord) != 0 ||
      (!(s.flags & kCustomChord) && s.chordCount != 0)) return false;
  const uint32_t used = s.gardenCount == 32 ? UINT32_MAX :
                        (1u << s.gardenCount) - 1u;
  if ((s.phraseStartMask & ~used) != 0 ||
      (s.gardenCount > 0 && !(s.phraseStartMask & 1u))) return false;
  for (int i = 0; i < s.gardenCount; ++i) {
    if ((s.phraseStartMask & (1u << i)) && s.delayMs[i] != 0) return false;
    if (legacy) {
      if (s.delayMs[i] > 5000) s.delayMs[i] = 5000;
      s.holdMs[i] = 420;  // old snapshots never recorded releases
      s.velocity[i] = 90;
    } else if (s.delayMs[i] > 5000 || s.holdMs[i] < 20 || s.holdMs[i] > 5000 ||
               s.velocity[i] == 0 || s.velocity[i] > 100) {
      return false;
    }
  }
  s.version = 2;
  s.bytes = sizeof(s);
  out = s;
  return true;
}

}  // namespace gk
