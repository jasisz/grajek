// INSTRUMENT mode: the microtonal grid.
//
// The leftmost keyboard column (col 0) is the control column:
//   row 0 (`)    — next scale
//   row 1 (tab)  — next timbre
//   row 2 (fn)   — IMU role: bend / filter / off
//   row 3 (ctrl) — base octave 55/110/220/440 Hz
// The remaining 13x4 keys play: column = scale step, bottom row = lowest
// interval (geometry in ga_scales.h).
#pragma once
#include <stdint.h>

#include "ga_scales.h"
#include "mode.h"

class ModeInstrument : public Mode {
 public:
  const char* name() const override { return "INSTRUMENT"; }
  void enter(ModeCtx&) override;
  void exit(ModeCtx&) override;
  void onKey(ModeCtx&, int col, int row, bool down) override;
  void tick(ModeCtx&, float dt) override;
  void draw(ModeCtx&) override;

 private:
  enum class ImuRole : uint8_t { Bend, Filter, Off };
  void applyImu(ModeCtx&);
  static const char* roleName(ImuRole r);

  ga::ScaleId scale_ = ga::ScaleId::EDO19;
  int preset_ = 0;
  int octave_ = 2;  // index into {55, 110, 220, 440}
  ImuRole imuRole_ = ImuRole::Bend;
  uint64_t held_ = 0;      // pressed keys (for drawing the grid)
  float imuShown_ = 0.0f;  // last bend/cutoff value shown on the LCD
  float imuTimer_ = 0.0f;
  int lastVoices_ = 0;
};
