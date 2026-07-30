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

// Configures I2S TX + the ES8311 codec (official M5Unified playback
// sequence), initializes the effect chain and starts the audio task on
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

// --- THE EAR: a walkie-talkie, not full duplex ---
// Hardware fact (one night of measuring, then one read of M5Unified): the
// factory mic path is an I2S MASTER on the very pads our TX drives, on a
// codec with no MCLK — two masters on one bus cannot exist, so 48 kHz
// full duplex never had a chance. Instead the ear opens in WINDOWS: the
// speaker stops, M5.Mic (16 kHz, the factory-proven path) captures the
// voice, and on close the codec is re-latched and the box ANSWERS through
// its own chain (the tape ages your voice, the strings halo it). The
// speaker being silent while the child sings was the design anyway.
// Nothing is ever stored beyond the 3 s answer buffer in RAM.
bool earAvailable();         // mic pins configured and TX alive
void earSetOpen(bool open);  // arm/disarm (SPIEW mode); disarm aborts a window
bool earIsOpen();            // armed?
bool earWindowOpen();        // hand the bus to the mic (core 1 only)
void earWindowClose();       // mic off, codec ritual, speaker back (core 1)
bool earWindowActive();
float earSampleRate();       // capture rate inside a window (16000)
float earLevel();            // voice envelope 0..~1 (telemetry + viz ring)
bool earVoicePresent();      // hysteresis gate over earLevel
// analysis window for the duet: monotonic count of captured mono samples
// (per window) and a copy of the freshest n samples (n <= 2048)
uint32_t earCaptured();
void earWindow(float* out, int n);
// the answer: replay the captured voice into the chain where the synth
// enters. Start returns false when nothing was captured / no buffer.
bool earAnswerStart();
bool earAnswerActive();

// Codec diagnostics over serial (main forwards single characters):
//   'r' ES8311 register dump, 'g' mic PGA to max, 'l' codec ADC->DAC
//   loopback toggle, 'p' raw G46 pad probe, 'x' mic capture stats,
//   'k' TX clock relatch ritual, 'e' toggle a listening window by hand,
//   'M' automatic 700 ms window + answer (full walkie-talkie cycle)
void audioDiag(char cmd);
// write any ES8311 register (serial command "Wrrvv" in hex)
void audioDiagWrite(uint8_t reg, uint8_t val);

// Latency chain: DMA blocks + one synthesis block. 96 frames = 2 ms and is
// divisible by 3 for the lo-fi echo bridge.
constexpr int kAudioFrames = 96;
constexpr int kAudioDmaDescs = 4;  // ~8 ms of DMA buffering

}  // namespace hal
