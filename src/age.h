// The age dial: the box grows with the child. A parent gesture (long-press
// BtnGO in the menu) cycles the tier; the choice persists in NVS.
//   Maluch (2-3): the whole keyboard plays pentatonic, no switches at all
//   Sredniak (4-6): grid + controls, but only the happy scales
//   Duzy (7+): everything
#pragma once

namespace age {

enum Tier { Maluch = 0, Sredniak = 1, Duzy = 2 };

void load();   // call once in setup()
Tier tier();
void cycle();  // next tier, saved immediately
const char* label(Tier t);  // "2-3" / "4-6" / "7+"

}  // namespace age
