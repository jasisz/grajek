// Audio output: I2S -> ES8311 -> NS4150B/speaker or the 3.5mm jack.
// We deliberately do NOT use M5.Speaker (its mixer adds buffering = latency);
// the full grajek chain runs in our own task pinned to core 0:
//   engine -> sympathetic strings -> echo tape (lo-fi 16 kHz bridge)
//          -> reverb -> I2S DMA
#pragma once
#include "ga_echo.h"
#include "ga_engine.h"
#include "ga_reverb.h"
#include "ga_strings.h"

namespace hal {

// Configures I2S TX + the ES8311 codec (official register sequence from
// M5Unified), initializes the effect chain and starts the audio task on
// core 0. Returns false on I2S/I2C errors.
bool audioInit(ga::Engine* engine);

// Chain accessors for modes / the ambient brain (control-thread safe —
// all module setters are atomic).
ga::SympatheticStrings& strings();
ga::EchoTape& echo();
ga::Reverb& reverb();
bool echoAvailable();  // false if the tape buffer could not be allocated

// Envelope of the dry synth (attack ~5 ms, release ~2 s), published by the
// audio task — the weather system reads it to know when the player is quiet.
float audioEnv();

// --- UCHO (mikrofon ES8311, full-duplex na tym samym I2S) ---
// Głos wchodzi w łańcuch dokładnie tam, gdzie synth (taśma go starzy, struny
// go otulają), a wyjście przycicha (duck), żeby pętla mik->głośnik->mik nie
// miała się jak zamknąć. Przetwarzanie działa TYLKO przy otwartym uchu
// (tryb ŚPIEW); nic nie jest nigdy zapisywane.
bool earAvailable();          // false gdy full-duplex RX nie wstał
void earSetOpen(bool open);   // otwiera/zamyka ucho (tryb ŚPIEW)
bool earIsOpen();
float earLevel();             // obwiednia głosu 0..~1 (telemetria + wskaźnik)
void earSetDuet(bool on);     // duet nuci -> duck lżejszy (słychać towarzysza)
// okno analizy dla duetu: monotoniczny licznik zebranych próbek mono @48 kHz
// i kopia najświeższych n próbek (n <= 2048)
uint32_t earCaptured();
void earWindow(float* out, int n);

// Diagnostyka kodeka po serialu (main przekazuje znaki z Serial):
//   'r' zrzut rejestrów ES8311, 'd' fuzja duplex (domyślna),
//   'm' oficjalny mic-only (głośnik zamilknie!), 'a' duplex rozszerzony
//   o rejestry z pełnego sterownika ESP-ADF, 'g' PGA mikrofonu na maks,
//   'e' otwórz/zamknij ucho bez wchodzenia w tryb ŚPIEW
void audioDiag(char cmd);
// zapis dowolnego rejestru ES8311 (komenda serialowa "Wrrvv" w hex)
void audioDiagWrite(uint8_t reg, uint8_t val);

// Latency chain: DMA blocks + one synthesis block. 96 frames = 2 ms and is
// divisible by 3 for the lo-fi echo bridge.
constexpr int kAudioFrames = 96;
constexpr int kAudioDmaDescs = 4;  // ~8 ms of DMA buffering

}  // namespace hal
