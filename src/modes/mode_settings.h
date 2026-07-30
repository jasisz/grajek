// Ekran USTAWIEŃ: jedyne miejsce, gdzie klawisze coś przełączają.
// Wchodzi się świadomie (5 w menu), więc podczas grania nie ma czego
// zepsuć. Cyfry 1-4 cyklują: skalę, barwę, oktawę, tło; GO wraca do menu.
#pragma once
#include "mode.h"

class ModeSettings : public Mode {
 public:
  const char* name() const override { return "USTAWIENIA"; }
  void enter(ModeCtx&) override;
  void onKey(ModeCtx&, int col, int row, bool down) override;
  void draw(ModeCtx&) override;
};
