// Tryb INSTRUMENT: klawiatura = tylko muzyka.
//
// Wszystkie 56 klawiszy gra, w każdym wieku — zero specjalnych klawiszy,
// dziecko nie ma czego się bać. Kolumna = stopień skali, dolny rząd =
// najniższy interwał (geometria w ga_scales.h). Skala/barwa/oktawa mieszkają
// w settings (ekran USTAWIEŃ w menu); przytrzymany GO zmienia barwę w locie.
//
// Gesty:
//   MACHANIE  — wiatr wspomnień: każdy zamach wyrywa z ogrodu nutkę, którą
//               dziecko naprawdę zagrało (czasem oktawę wyżej); siła zamachu
//               = głośność. Klawisze robią nowe nuty, ruch gra stare — nie
//               konkurują. Pusty ogród: zapasowa drabinka skali.
//   PRZECHYŁ  — na boki: jasność brzmienia (filtr); do/od siebie: głębia
//               przestrzeni (pogłos+echo). Oba wygładzone (~0.4 s), stałe,
//               bez żadnych przełączników
//
// Ekran to wizualizacja "nocna łąka" (viz.h) — bez tekstu podczas grania.
#pragma once
#include <stdint.h>

#include "mode.h"

class ModeInstrument : public Mode {
 public:
  const char* name() const override { return "INSTRUMENT"; }
  void enter(ModeCtx&) override;
  void exit(ModeCtx&) override;
  void onKey(ModeCtx&, int col, int row, bool down) override;
  void onGoHold(ModeCtx&) override;  // przytrzymany GO = następna barwa
  void tick(ModeCtx&, float dt) override;
  void draw(ModeCtx&) override;

 private:
  void imuStep(ModeCtx&);
  void triggerChime(ModeCtx&, float energy, float dir);

  // Gesture state (gravity filter, tilts, chime ladder) lives as file-scope
  // statics in mode_instrument.cpp, NOT here: INSTRUMENT and SPIEW are two
  // objects but one physical instrument — switching between them must not
  // reset the smoothed gravity and make the sound jump.
};
