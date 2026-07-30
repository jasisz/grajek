#include "ga_scales.h"
#include <math.h>

namespace ga {

namespace {

struct Ratio { int num, den; };

// 14 ascending 11-limit intervals within the octave, in the spirit of
// Partch's tonality diamond.
constexpr Ratio kJi11[kGridCols] = {
    {1, 1}, {12, 11}, {9, 8}, {7, 6}, {6, 5}, {5, 4}, {4, 3},
    {11, 8}, {3, 2}, {14, 9}, {8, 5}, {5, 3}, {7, 4}, {11, 6},
};

struct Ji11Cents {
  float c[kGridCols];
  Ji11Cents() {
    for (int i = 0; i < kGridCols; ++i)
      c[i] = 1200.0f * log2f((float)kJi11[i].num / (float)kJi11[i].den);
  }
};

const float* ji11Cents() {
  static Ji11Cents t;
  return t.c;
}

int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

// The EDO grids climb by fourths, so their 4x14 keys span only ~1.7-2.3
// octaves against ~3.3-3.9 for the octave-row scales — switching from PENTA
// to any EDO dropped the whole keyboard by more than an octave ("EDO sounds
// too low"). Lifting the EDO grids one octave puts their centre back where
// the child's ear already lives.
float registerLift(ScaleId s) {
  switch (s) {
    case ScaleId::EDO12:
    case ScaleId::EDO19:
    case ScaleId::EDO31: return 1200.0f;
    default:             return 0.0f;
  }
}

// The "happy" subsets, in pure intonation:
// major pentatonic 1/1 9/8 5/4 3/2 5/3 — the kalimba/wind-chime scale
const float kPenta[5] = {0.0f, 203.9f, 386.3f, 702.0f, 884.4f};
// Ptolemy's intense diatonic (just major): 1/1 9/8 5/4 4/3 3/2 5/3 15/8
const float kMajor[7] = {0.0f, 203.9f, 386.3f, 498.0f,
                         702.0f, 884.4f, 1088.3f};

float stepTable(const float* t, int n, int degree) {
  const int oct = degree / n;
  return t[degree % n] + 1200.0f * (float)oct;
}

}  // namespace

const ScaleInfo& scaleInfo(ScaleId s) {
  static const ScaleInfo infos[(int)ScaleId::Count] = {
      {"12-EDO", "row = +5 steps (fourth)"},
      {"19-EDO", "row = +8 steps (fourth)"},
      {"31-EDO", "row = +13 steps (fourth)"},
      {"JI 11-limit", "14 ratios/octave, row = +octave"},
      {"PENTA JI", "major pentatonic, row = +2 steps (thirds)"},
      {"MAJOR JI", "just major, row = +2 steps (thirds)"},
  };
  int i = (int)s;
  if (i < 0 || i >= (int)ScaleId::Count) i = 0;
  return infos[i];
}

int scaleStepsPerOctave(ScaleId s) {
  switch (s) {
    case ScaleId::EDO12: return 12;
    case ScaleId::EDO19: return 19;
    case ScaleId::EDO31: return 31;
    case ScaleId::JI11:  return kGridCols;
    case ScaleId::PENTA: return 5;
    case ScaleId::MAJOR: return 7;
    default:             return 12;
  }
}

float scaleStepCents(ScaleId s, int step) {
  if (step < 0) step = 0;
  const float lift = registerLift(s);
  switch (s) {
    case ScaleId::EDO12: return lift + (float)step * 100.0f;
    case ScaleId::EDO19: return lift + (float)step * (1200.0f / 19.0f);
    case ScaleId::EDO31: return lift + (float)step * (1200.0f / 31.0f);
    case ScaleId::JI11:
      return ji11Cents()[step % kGridCols] +
             1200.0f * (float)(step / kGridCols);
    case ScaleId::PENTA: return stepTable(kPenta, 5, step);
    case ScaleId::MAJOR: return stepTable(kMajor, 7, step);
    default:             return (float)step * 100.0f;
  }
}

float gridToCents(ScaleId s, int col, int row) {
  col = clampi(col, 0, kGridCols - 1);
  row = clampi(row, 0, kGridRows - 1);
  const float lift = registerLift(s);
  switch (s) {
    case ScaleId::EDO12: return lift + (float)(col + row * 5) * 100.0f;
    case ScaleId::EDO19: return lift + (float)(col + row * 8) * (1200.0f / 19.0f);
    case ScaleId::EDO31: return lift + (float)(col + row * 13) * (1200.0f / 31.0f);
    case ScaleId::JI11:  return ji11Cents()[col] + (float)row * 1200.0f;
    case ScaleId::PENTA: return stepTable(kPenta, 5, col + row * 2);
    case ScaleId::MAJOR: return stepTable(kMajor, 7, col + row * 2);
    default:             return (float)col * 100.0f;
  }
}

}  // namespace ga
