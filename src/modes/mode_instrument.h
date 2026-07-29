// INSTRUMENT mode: the microtonal grid + the toss gesture.
//
// The leftmost keyboard column (col 0) is the control column:
//   row 0 (`)    — next scale
//   row 1 (tab)  — next timbre
//   row 2 (fn)   — IMU tilt role: bend / filter / off
//   row 3 (ctrl) — TAP: base octave 55/110/220/440 Hz
//                  HOLD + grid keys: pick the background chord (max 4 notes)
// The remaining 13x4 keys play: column = scale step, bottom row = lowest
// interval (geometry in ga_scales.h).
//
// The TOSS is always armed, independent of the tilt role: free fall
// (|a| ~ 0 g) lifts the whole music in a glissando; the catch (an |a| spike)
// lands it re-rooted on a rung of the JI ladder — flight time picks the
// rung, the spin direction in flight picks up vs down.
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
  enum class TossPhase : uint8_t { Grounded, Flight };

  void applyTilt(ModeCtx&, float ax, float az);
  void applyRoot(ModeCtx&);
  void land(ModeCtx&, float flightSec);
  static const char* roleName(ImuRole r);

  ga::ScaleId scale_ = ga::ScaleId::PENTA;  // the happy default
  int preset_ = 3;  // CHIME — the "pretty by itself" default
  int octave_ = 2;  // index into {55, 110, 220, 440}
  ImuRole imuRole_ = ImuRole::Bend;
  float centerCents_ = 0.0f;  // accumulated toss modulation

  TossPhase toss_ = TossPhase::Grounded;
  int freefall_ = 0;
  uint32_t tossT0Ms_ = 0;
  float spinDeg_ = 0.0f;
  float lastLandCents_ = 0.0f;  // for the LCD

  bool ctrlHeld_ = false;  // (0,3) held = background pick modifier
  bool ctrlUsed_ = false;

  uint64_t held_ = 0;      // pressed keys (for drawing the grid)
  float imuShown_ = 0.0f;  // last bend/cutoff value shown on the LCD
  float imuTimer_ = 0.0f;
  int lastVoices_ = 0;
};
