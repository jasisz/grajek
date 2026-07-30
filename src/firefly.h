// The firefly in the box: the WS2812 on G21 is a physical firefly from
// the meadow. A note flashes it in the color of its pitch (the same
// octave fold as the X axis on screen — the same note is always the same
// color), a replayed memory glows it softly. It lights up ONLY as a
// consequence of sound, and every sound traces back to a human act —
// idle means dark, never a "come play with me" blink. At night, with the
// screen off in a pocket, the box is a tiny lantern playing the child's
// own melody.
//
// Battery dusk lives here too: no warnings, no nagging — the firefly
// simply grows dimmer as the battery empties, like a real one at dawn.
#pragma once

namespace firefly {

void note(float cents, float vel);  // a played note flashes
void ghost(float cents);            // a memory glows, longer and softer
void tick();                        // decay + battery dusk; every loop pass
void hello();                       // one soft waking glow at power-on
void selfTest();                    // serial diag 'f': R/G/B/W, ~2 s, blocking

}  // namespace firefly
