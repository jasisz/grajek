// Scales and the 4x14 grid geometry.
//
// Isomorphic layout: column = +1 scale step, row = an interval close to a
// fourth (for EDO) or +1 octave (for just intonation, where 14 columns span
// one octave).
//  - 12-EDO: row = +5 steps (500 c)
//  - 19-EDO: row = +8 steps (505.3 c)
//  - 31-EDO: row = +13 steps (503.2 c)
//    (all EDO grids are lifted +1 octave: fourth-rows span far less than
//     octave-rows, and without the lift switching PENTA -> EDO dropped the
//     whole keyboard by over an octave)
//  - JI 11-limit (Partch spirit): 14 ratios per octave, row = +1200 c
//  - PENTA JI (major pentatonic 1/1 9/8 5/4 3/2 5/3) and MAJOR JI
//    (Ptolemy's diatonic): row = +2 scale steps, so vertical neighbours are
//    thirds and a whole column is a pretty chord — the "happy" scales where
//    no pair of keys can clash
//
#pragma once
#include <stdint.h>

namespace ga {

constexpr int kGridCols = 14;
constexpr int kGridRows = 4;

enum class ScaleId : uint8_t {
  EDO12, EDO19, EDO31, JI11, PENTA, MAJOR,
  // WOLF — the anti-scale, hiding at the strange end of the settings list:
  // 62 c column steps, 637 c rows; the only "fifth" in the grid is the
  // historically correct, howling 682 c wolf. Nothing is pure. Born from
  // a forum dare: "can you make one that is always dissonant though?"
  WOLF,
  Count
};

struct ScaleInfo {
  const char* name;   // short name for the LCD
  const char* desc;   // row-geometry description
};

const ScaleInfo& scaleInfo(ScaleId s);

// col: 0..13, row: 0..3 (0 = bottom row). Result in cents relative to BaseHz.
float gridToCents(ScaleId s, int col, int row);

// Absolute scale step -> cents (step may span octaves). The melodic ladder
// for gesture-triggered chimes: step N is always the Nth note of the scale.
float scaleStepCents(ScaleId s, int step);
int scaleStepsPerOctave(ScaleId s);

}  // namespace ga
