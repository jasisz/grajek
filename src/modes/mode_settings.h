// The SETTINGS screen: the only place where keys switch anything. Entered
// deliberately (a short GO press while playing), so nothing can be broken
// mid-play. Digits 1-5 cycle: scale, timbre, octave, background and scene;
// GO returns to playing. Entering also saves the soul (see soul.h).
#pragma once
#include "mode.h"

class ModeSettings : public Mode {
 public:
  const char* name() const override { return "USTAWIENIA"; }
  void enter(ModeCtx&) override;
  void exit(ModeCtx&) override;
  void onKey(ModeCtx&, int col, int row, bool down) override;
  void tick(ModeCtx&, float dt) override;
  void draw(ModeCtx&) override;

 private:
  float imuTimer_ = 0.0f;
};
