// The firefly in the box: the WS2812 on G21 is a physical firefly from
// the meadow. A note flashes it in the color of its pitch (the same
// octave fold as the X axis on screen — the same note is always the same
// color), a replayed memory glows it softly. It lights up ONLY as a
// consequence of sound, and every sound traces back to a human act —
// idle means dark, never a "come play with me" blink. On this board the LED
// and LCD backlight share a power rail, so bedtime extinguishes both.
//
// Battery dusk lives here too: no warnings, no nagging — the firefly
// simply grows dimmer as the battery empties, like a real one at dawn.
#pragma once

namespace firefly {

void note(float cents, float vel);  // a played note flashes
void ghost(float cents);            // a memory glows, longer and softer
void tick();                        // decay + battery dusk; every loop pass
void hello();                       // one soft waking glow at power-on
void setSleeping(bool sleeping);    // black out with the shared LCD/LED rail

}  // namespace firefly
