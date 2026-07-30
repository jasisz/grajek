// DUET, walkie-talkie edition: while a listening window is open the box
// tracks the singer's pitch (autocorrelation over the ear ring, 80-500 Hz),
// snaps every stable note to the current grid, plants it in the memory
// garden, and RECORDS a track of consonant companion tones a third below
// the voice. It cannot hum live — the speaker is off while the ear owns
// the bus — so SPIEW replays the track during the ANSWER, in time with the
// captured voice. The human voice itself stays untouched.
#pragma once
#include <stdint.h>

namespace duet {

void reset();  // fresh window: clears the analysis state and the track
void tick();   // call every loop pass while the window is open

// the companion track: sample offsets are in mic samples from window start
int trackCount();
uint32_t trackOffset(int i);
float trackCents(int i);

}  // namespace duet
