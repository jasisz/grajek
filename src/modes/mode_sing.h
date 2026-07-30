// Tryb ŚPIEW: to samo granie co INSTRUMENT (klawisze, machanie, przechył),
// ale z otwartym uchem — głos wchodzi w taśmę i struny, a DUET nuci
// spółbrzmiący głos pod śpiewem. Wejście w tryb = jedyna zgoda na słuchanie;
// wyjście zamyka ucho. Nic nie jest nigdy zapisywane.
#pragma once
#include "mode_instrument.h"

class ModeSing : public ModeInstrument {
 public:
  const char* name() const override { return "SPIEW (duet)"; }
  void enter(ModeCtx&) override;
  void exit(ModeCtx&) override;
  void tick(ModeCtx&, float dt) override;
};
