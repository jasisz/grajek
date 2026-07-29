// Audio output: I2S -> ES8311 -> NS4150B/speaker or the 3.5mm jack.
// We deliberately do NOT use M5.Speaker (its mixer adds buffering = latency);
// the grajek_audio engine writes straight into I2S DMA from its own task
// pinned to core 0.
#pragma once
#include "ga_engine.h"

namespace hal {

// Configures I2S TX + the ES8311 codec (official register sequence from
// M5Unified) and starts the audio task on core 0. Returns false on I2S/I2C
// errors.
bool audioInit(ga::Engine* engine);

// Latency chain: DMA blocks (kDmaDescs * kFrames frames) + one synthesis block.
constexpr int kAudioFrames = 64;   // 1.33 ms @ 48 kHz
constexpr int kAudioDmaDescs = 4;  // ~5.3 ms of DMA buffering

}  // namespace hal
