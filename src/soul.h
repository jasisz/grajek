// The soul: what the box keeps overnight — the memory garden (short phrases
// with the child's pitches and timing), the last stable pulse, and an optional
// custom background supplied by alternate controllers. Saved atomically at
// pauses (five quiet seconds, settings, goodnight), restored at boot; then the
// box greets you with ONE remembered note. Design law no. 5 made real on
// the device, not just the host.
#pragma once

namespace soul {

void load();  // call once in setup, after settings are applied
bool save();  // true on success/no-op; false lets deferred callers retry

}  // namespace soul
