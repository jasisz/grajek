// Tryb ŚPIEW — krótkofalówka: to samo granie co INSTRUMENT (klawisze,
// machanie, przechył), ale gdy przestajesz grać, pudełko oddaje głośnik
// uchu i SŁUCHA (M5.Mic — jedna magistrala I2S, patrz audio_out.h).
// Zaśpiewaj i zamilknij: głośnik wraca, a pudełko ODPOWIADA twoim głosem
// przez taśmę i struny, nucąc pod nim spółbrzmiący głos (duet z nagraną
// ścieżką). Każdy klawisz natychmiast zamyka okno — klawisze zawsze
// wygrywają. Wejście w tryb = jedyna zgoda na słuchanie; wyjście zamyka
// ucho. Poza 3-sekundowym buforem odpowiedzi nic nie jest zapisywane.
#pragma once
#include <stdint.h>

#include "mode_instrument.h"

class ModeSing : public ModeInstrument {
 public:
  const char* name() const override { return "SPIEW (duet)"; }
  void enter(ModeCtx&) override;
  void exit(ModeCtx&) override;
  void onKey(ModeCtx&, int col, int row, bool down) override;
  void tick(ModeCtx&, float dt) override;

 private:
  enum class St : uint8_t { Idle, Listen, Answer };
  St st_ = St::Idle;
  uint32_t lastKeyMs_ = 0;
  uint32_t lastVoiceMs_ = 0;
  uint32_t cooldownUntil_ = 0;
  uint32_t answerStartMs_ = 0;
  uint32_t listenStartMs_ = 0;
  bool heardVoice_ = false;
  bool dormant_ = false;  // a voiceless window fell asleep; a key wakes it
  int trackIdx_ = 0;
  int heldCount_ = 0;  // no listening window while a key is held
};
