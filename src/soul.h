// The soul: what the box keeps overnight — the memory garden (the notes
// the child really played), a hand-picked background chord, and the last
// stable pulse period. Saved to NVS at natural pauses (entering settings,
// the goodnight ritual), restored at boot; a few seconds after waking the
// box greets you with ONE remembered note. Design law no. 5 made real on
// the device, not just the host.
#pragma once

namespace soul {

void load();  // call once in setup, after settings are applied
void save();  // cheap when nothing changed (fingerprint check)

}  // namespace soul
