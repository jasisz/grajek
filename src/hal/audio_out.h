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
bool audioReady();

// Chain accessors for modes / the ambient brain (control-thread safe —
// all module setters are atomic).
ga::SympatheticStrings& strings();
ga::EchoTape& echo();
ga::Reverb& reverb();
bool echoAvailable();  // false if the tape buffer could not be allocated

// Envelope of the dry synth (attack ~5 ms, release ~2 s), published by the
// audio task — the weather system reads it to know when the player is quiet.
float audioEnv();

// Briefly fade and park the audio task around flash/NVS commits. Returns true
// only when the task acknowledged the park; pair with audioResumeAfterFlash().
bool audioParkForFlash();
void audioResumeAfterFlash();

// Latency chain: DMA blocks + one synthesis block. 96 frames = 2 ms and is
// divisible by 3 for the lo-fi echo bridge.
constexpr int kAudioFrames = 96;
constexpr int kAudioDmaDescs = 4;  // ~8 ms of DMA buffering

}  // namespace hal
