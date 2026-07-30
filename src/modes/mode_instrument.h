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

  // przechyły: wygładzone pozycje -1..1 (boki = jasność, do/od = przestrzeń)
  float tiltNorm_ = 0.0f;
  float tiltFbNorm_ = 0.0f;
  // machanie: grawitacja odfiltrowana wolnym LP, detekcja zamachów z histerezą
  float gravX_ = 0.0f, gravY_ = 0.0f, gravZ_ = 1.0f;
  float shakeEnv_ = 0.0f;   // wygładzona energia machania (tłumi przechył)
  bool swingArmed_ = true;  // histereza: nowy dzwonek dopiero po opadnięciu
  uint32_t lastChimeMs_ = 0;
  int chimeStep_ = 0;       // pozycja melodii grzechotki na drabince skali
  int chimeSlot_ = 0;       // rotacja id nut dzwonków

  // zaplanowane noteOff dla dzwonków (krótkie nuty, wybrzmiewają release'em)
  struct PendingOff { uint32_t atMs; int32_t id; };
  PendingOff pending_[8];
  int pendingCount_ = 0;

  uint64_t held_ = 0;  // wciśnięte klawisze siatki
  float imuTimer_ = 0.0f;
};
