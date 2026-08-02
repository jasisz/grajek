// Tryb INSTRUMENT: klawiatura = tylko muzyka.
//
// Wszystkie 56 klawiszy gra, w każdym wieku — zero specjalnych klawiszy,
// dziecko nie ma czego się bać. Kolumna = stopień skali, dolny rząd =
// najniższy interwał (geometria w ga_scales.h). Skala/barwa/oktawa mieszkają
// w settings (ekran USTAWIEŃ w menu); przytrzymany GO zmienia barwę w locie.
//
// Gesty:
//   MACHANIE  — wiatr wspomnień: każdy zamach wyrywa z ogrodu krótką frazę
//               z rytmem dziecka (czasem całą przenosi); siła zamachu =
//               głośność. Klawisze robią nowe frazy, ruch gra stare — nie
//               konkurują. Pusty ogród: zapasowa drabinka skali.
//   PRZECHYŁ  — na boki: jasność brzmienia (filtr); do/od siebie: głębia
//               przestrzeni (pogłos+echo). Oba wygładzone (~0.4 s), stałe,
//               bez żadnych przełączników
//
// Ekran to jedna z pięciu scen wizualizacji (viz.h) — bez tekstu podczas
// grania; scenę wybiera się w ustawieniach.
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
  void playChimeNote(ModeCtx&, float cents, float velocity);
  void windPhraseStep(ModeCtx&, uint32_t nowMs);
  void windPhraseCancel(ModeCtx&);

  // Gesture state (gravity filter, tilts, chime ladder) lives as file-scope
  // statics in mode_instrument.cpp so a settings visit does not reset it.
};
