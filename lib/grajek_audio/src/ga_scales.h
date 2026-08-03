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
#include <stddef.h>
#include <stdint.h>

namespace ga {

constexpr int kGridCols = 14;
constexpr int kGridRows = 4;

enum class ScaleId : uint8_t {
  // These values are persisted by the host. Never renumber an existing ID.
  EDO12 = 0,
  EDO19 = 1,
  EDO31 = 2,
  JI11 = 3,
  PENTA = 4,
  MAJOR = 5,
  // WOLF — the anti-scale, hiding at the strange end of the settings list:
  // 62 c column steps, 637 c rows; the only "fifth" in the grid is the
  // historically correct, howling 682 c wolf. Nothing is pure. Born from
  // a forum dare: "can you make one that is always dissonant though?"
  WOLF = 6,
  Count = 7
};

struct ScaleInfo {
  const char* name;   // short name for the LCD
  const char* desc;   // row-geometry description
};

// A scale "plugin" is deliberately static: plain metadata and callbacks,
// with no allocation, constructors or platform dependencies. Callback inputs
// are already normalized by gridToCents()/scaleStepCents(); normal callers
// should use those compatibility helpers rather than invoking them directly.
using ScaleStepFn = float (*)(int step);
using ScaleGridFn = float (*)(int col, int row);

struct ScalePlugin {
  ScaleId id;                  // stable persisted identity
  ScaleInfo info;              // stable, non-localized host/fallback text
  uint8_t stepsPerOctave;
  ScaleStepFn stepFn;
  ScaleGridFn gridFn;
};

// Presentation/discovery order. This order is also the legacy firmware NVS
// mapping for the "skala" index, so append or migrate stored data before ever
// reordering existing entries.
size_t scalePluginCount();
const ScalePlugin& scalePluginAt(size_t index);
const ScalePlugin& scalePlugin(ScaleId id);
size_t scalePluginIndex(ScaleId id);

// Compatibility API kept for existing host and firmware callers.
const ScaleInfo& scaleInfo(ScaleId s);

// col: 0..13, row: 0..3 (0 = bottom row). Result in cents relative to BaseHz.
float gridToCents(ScaleId s, int col, int row);

// Absolute scale step -> cents (step may span octaves). The melodic ladder
// for gesture-triggered chimes: step N is always the Nth note of the scale.
float scaleStepCents(ScaleId s, int step);
int scaleStepsPerOctave(ScaleId s);

}  // namespace ga
