#include "audio_out.h"

#include <M5Unified.h>
#include <driver/i2s_std.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <atomic>

#include "board_pins.h"
#include "ga_bridge.h"

namespace {

// The lo-fi bridge averages triplets into a fixed-size low-rate scratch —
// this is the contract its comment states but never enforced.
static_assert(hal::kAudioFrames % 3 == 0 &&
                  hal::kAudioFrames / 3 <=
                      ga::LofiBridge3<ga::EchoTape>::kMaxLow,
              "kAudioFrames must fit the lo-fi echo bridge");

i2s_chan_handle_t s_tx = nullptr;
ga::Engine* s_engine = nullptr;
std::atomic<bool> s_audioReady{false};
std::atomic<bool> s_txPaused{false};  // request: fade and park for NVS
std::atomic<bool> s_txParked{false};  // ack from the audio task

// The effect chain lives here, in static state:
//  - reverb: ~55 KB of comb/allpass floats (.bss)
//  - strings: ~9 KB
//  - echo tape: heap-allocated, 3 s at 16 kHz (96 KB) behind a rate bridge
ga::SympatheticStrings s_strings;
ga::Reverb s_reverb;
ga::EchoTape s_echo;
ga::LofiBridge3<ga::EchoTape> s_echoBridge;
int16_t* s_tape = nullptr;
constexpr int kEchoRate = 16000;
constexpr float kEchoSeconds = 3.0f;

// Speaker voicing for the 3 cm driver, applied to the final mix:
//  - 2nd-order high-pass at 180 Hz: energy the membrane cannot reproduce
//    only distorts trying — remove it and the mids come out cleaner (the
//    engine's virtual-pitch bass already keeps low notes AUDIBLE);
//  - gentle presence shelf (+2.5 dB above ~2.5 kHz) for clarity.
// The 3.5 mm jack gets the same voicing (hardware mute only) — acceptable:
// it is mild, and the box is tuned for its own speaker first.
struct Biquad {
  float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
  inline float process(float x) {
    const float y = b0 * x + z1;
    z1 = b1 * x - a1 * y + z2;
    z2 = b2 * x - a2 * y;
    return y;
  }
};
Biquad s_spkHp, s_spkShelf;

void speakerVoicingInit(float sr) {
  {  // RBJ high-pass, f=180 Hz, Q=0.707
    const float w = 6.2831853f * 180.0f / sr;
    const float cw = cosf(w), alpha = sinf(w) / (2.0f * 0.707f);
    const float a0 = 1.0f + alpha;
    s_spkHp.b0 = (1.0f + cw) * 0.5f / a0;
    s_spkHp.b1 = -(1.0f + cw) / a0;
    s_spkHp.b2 = (1.0f + cw) * 0.5f / a0;
    s_spkHp.a1 = -2.0f * cw / a0;
    s_spkHp.a2 = (1.0f - alpha) / a0;
  }
  {  // RBJ high shelf, f=2500 Hz, +2.5 dB, S=0.7
    const float A = powf(10.0f, 2.5f / 40.0f);
    const float w = 6.2831853f * 2500.0f / sr;
    const float cw = cosf(w);
    const float alpha =
        sinf(w) * 0.5f * sqrtf((A + 1.0f / A) * (1.0f / 0.7f - 1.0f) + 2.0f);
    const float sqA2al = 2.0f * sqrtf(A) * alpha;
    const float a0 = (A + 1.0f) - (A - 1.0f) * cw + sqA2al;
    s_spkShelf.b0 = A * ((A + 1.0f) + (A - 1.0f) * cw + sqA2al) / a0;
    s_spkShelf.b1 = -2.0f * A * ((A - 1.0f) + (A + 1.0f) * cw) / a0;
    s_spkShelf.b2 = A * ((A + 1.0f) + (A - 1.0f) * cw - sqA2al) / a0;
    s_spkShelf.a1 = 2.0f * ((A - 1.0f) - (A + 1.0f) * cw) / a0;
    s_spkShelf.a2 = ((A + 1.0f) - (A - 1.0f) * cw - sqA2al) / a0;
  }
}

// Safety ceiling: a hard cap on the final output (~-5 dBFS after a soft
// clip). This instrument is for a kid — no combination of layers may ever
// get loud enough to hurt, on the speaker or on headphones. Kept digital
// (the ES8311 DAC stays at 0 dB for SNR); raise cautiously if ever needed.
constexpr float kOutputCeiling = 0.55f;

std::atomic<float> s_env{0.0f};

bool es8311Write(uint8_t reg, uint8_t val) {
  return M5.In_I2C.writeRegister8(hal::kEs8311Addr, reg, val, 400000);
}

// 1:1 with M5Unified 0.2.10, _speaker_enabled_cb_cardputer_adv — the official
// playback-path configuration for this board.
bool es8311InitPlayback() {
  static constexpr uint8_t seq[][2] = {
      {0x00, 0x80},  // RESET: CSM power on
      {0x01, 0xB5},  // CLOCK_MANAGER: MCLK=BCLK
      {0x02, 0x18},  // CLOCK_MANAGER: MULT_PRE
      {0x0D, 0x01},  // SYSTEM: power up analog
      {0x12, 0x00},  // SYSTEM: power up DAC
      {0x13, 0x10},  // SYSTEM: output to HP drive (NS4150B input)
      {0x32, 0xBF},  // DAC volume: 0 dB
      {0x37, 0x08},  // bypass DAC equalizer
  };
  for (auto& rv : seq)
    if (!es8311Write(rv[0], rv[1])) return false;
  return true;
}

void audioTask(void*) {
  static float fbuf[hal::kAudioFrames];
  static int16_t mono[hal::kAudioFrames];
  static int16_t stereo[hal::kAudioFrames * 2];
  float envLocal = 0.0f;
  float parkGain = 1.0f;  // ~8 ms fade before an NVS commit
  for (;;) {
    const bool pauseReq = s_txPaused.load(std::memory_order_relaxed);
    if (pauseReq && parkGain <= 0.0f) {
      // Keep draining engine events while flash writes temporarily park the
      // output. Effects stay frozen so the tape does not record the silence.
      s_txParked.store(true, std::memory_order_release);
      s_engine->process(fbuf, hal::kAudioFrames);
      vTaskDelay(pdMS_TO_TICKS(2));
      continue;
    }

    // Epoch ack: only the audio task clears this flag, so a quick sequence of
    // saves cannot mistake an acknowledgement from the previous park.
    s_txParked.store(false, std::memory_order_relaxed);

    s_engine->process(fbuf, hal::kAudioFrames);
    // publish the dry-synth envelope for the weather system
    for (int i = 0; i < hal::kAudioFrames; ++i) {
      const float a = fabsf(fbuf[i]);
      envLocal += (a > envLocal ? 0.004f : 0.00001f) * (a - envLocal);
    }
    s_env.store(envLocal, std::memory_order_relaxed);

    s_strings.process(fbuf, fbuf, hal::kAudioFrames);
    if (s_tape) s_echoBridge.process(fbuf, hal::kAudioFrames);
    s_reverb.process(fbuf, hal::kAudioFrames);

    // Park fade: ramp the tail down over four blocks instead of letting a
    // flash-backed settings save produce a click.
    if (pauseReq) {
      const float g1 = parkGain - 0.25f < 0.0f ? 0.0f : parkGain - 0.25f;
      for (int i = 0; i < hal::kAudioFrames; ++i)
        fbuf[i] *= parkGain +
                   (g1 - parkGain) * (float)i * (1.0f / hal::kAudioFrames);
      parkGain = g1;
    } else if (parkGain < 1.0f) {
      // The same four-block ramp in the other direction prevents a click
      // when the NVS commit hands audio back.
      const float g1 = parkGain + 0.25f > 1.0f ? 1.0f : parkGain + 0.25f;
      for (int i = 0; i < hal::kAudioFrames; ++i)
        fbuf[i] *= parkGain +
                   (g1 - parkGain) * (float)i * (1.0f / hal::kAudioFrames);
      parkGain = g1;
    } else {
      parkGain = 1.0f;
    }

    // Speaker voicing, then the glue clip and safety ceiling.
    for (int i = 0; i < hal::kAudioFrames; ++i) {
      const float v = s_spkShelf.process(s_spkHp.process(fbuf[i]));
      fbuf[i] = ga::softClip(v) * kOutputCeiling;
    }

    ga::Engine::toInt16(fbuf, mono, hal::kAudioFrames);
    // The ES8311 DAC is mono but the I2S frame is stereo — duplicate the channel
    for (int i = 0; i < hal::kAudioFrames; ++i) {
      stereo[2 * i] = mono[i];
      stereo[2 * i + 1] = mono[i];
    }
    size_t written = 0;
    // Blocks on DMA — this is our clock; the loop never spins dry.
    // Note: the last arg is a timeout in MILLISECONDS, not RTOS ticks.
    i2s_channel_write(s_tx, stereo, sizeof(stereo), &written, 1000);
  }
}

bool requestTxPark() {
  // Wait out the previous park epoch first. Otherwise a fast save/open could
  // mistake its stale acknowledgement for the new request.
  for (int i = 0; i < 100 && s_txParked.load(std::memory_order_acquire); ++i)
    vTaskDelay(1);
  if (s_txParked.load(std::memory_order_acquire)) return false;
  s_txPaused.store(true, std::memory_order_relaxed);
  for (int i = 0; i < 100 && !s_txParked.load(std::memory_order_acquire); ++i)
    vTaskDelay(1);
  if (s_txParked.load(std::memory_order_acquire)) return true;
  s_txPaused.store(false, std::memory_order_relaxed);
  return false;
}

}  // namespace

namespace hal {

ga::SympatheticStrings& strings() { return s_strings; }
ga::EchoTape& echo() { return s_echo; }
ga::Reverb& reverb() { return s_reverb; }
bool echoAvailable() { return s_tape != nullptr; }
bool audioReady() { return s_audioReady.load(std::memory_order_acquire); }
float audioEnv() { return s_env.load(std::memory_order_relaxed); }

bool audioParkForFlash() {
  if (!audioReady() || !s_tx || s_txPaused.load(std::memory_order_relaxed))
    return false;
  if (requestTxPark()) return true;
  Serial.println("grajek: audio nie zaparkowalo przed zapisem NVS");
  return false;
}

void audioResumeAfterFlash() {
  s_txPaused.store(false, std::memory_order_relaxed);
}

bool audioInit(ga::Engine* engine) {
  s_engine = engine;
  s_audioReady.store(false, std::memory_order_relaxed);

  // TX-only I2S1 master: this task owns the playback bus for its lifetime.
  i2s_std_config_t txCfg = {};
  txCfg.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG((uint32_t)engine->sampleRate());
  txCfg.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
      I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
  txCfg.gpio_cfg.mclk = I2S_GPIO_UNUSED;  // no MCLK line — codec runs MCLK=BCLK
  txCfg.gpio_cfg.bclk = (gpio_num_t)kPinI2sBclk;
  txCfg.gpio_cfg.ws = (gpio_num_t)kPinI2sWs;
  txCfg.gpio_cfg.dout = (gpio_num_t)kPinI2sDout;
  txCfg.gpio_cfg.din = I2S_GPIO_UNUSED;
  txCfg.gpio_cfg.invert_flags.mclk_inv = false;
  txCfg.gpio_cfg.invert_flags.bclk_inv = false;
  txCfg.gpio_cfg.invert_flags.ws_inv = false;

  i2s_chan_config_t chanCfg =
      I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
  chanCfg.dma_desc_num = kAudioDmaDescs;
  chanCfg.dma_frame_num = kAudioFrames;
  // A parked task deliberately stops feeding TX while NVS owns flash. Clear
  // each sent descriptor so an underrun is real silence instead of a tiny
  // stale waveform looping as an audible buzz.
  chanCfg.auto_clear_after_cb = true;
  if (i2s_new_channel(&chanCfg, &s_tx, nullptr) != ESP_OK) return false;
  if (i2s_channel_init_std_mode(s_tx, &txCfg) != ESP_OK) {
    i2s_del_channel(s_tx);
    s_tx = nullptr;
    return false;
  }

  // Codec at DEAD clocks (like M5Unified) — the ES8311 only latches its
  // configuration on a clean clock start.
  if (!es8311InitPlayback()) {
    i2s_del_channel(s_tx);
    s_tx = nullptr;
    return false;
  }
  vTaskDelay(pdMS_TO_TICKS(20));

  // The freshly allocated DMA ring has unspecified contents. Fill every
  // descriptor with silence before clocks start, so boot cannot expose a
  // stale fragment as a click or buzz before the audio task's first write.
  static const int16_t silence[kAudioFrames * 2] = {};
  for (int i = 0; i < kAudioDmaDescs; ++i) {
    size_t loaded = 0;
    if (i2s_channel_preload_data(s_tx, silence, sizeof(silence), &loaded) !=
            ESP_OK ||
        loaded == 0)
      break;
  }

  if (i2s_channel_enable(s_tx) != ESP_OK) {
    i2s_del_channel(s_tx);
    s_tx = nullptr;
    return false;
  }

  // --- effect chain ---
  const float sr = engine->sampleRate();
  speakerVoicingInit(sr);
  s_strings.init(sr);
  s_strings.setRootHz(220.0f);
  s_reverb.init(sr);
  s_reverb.setWet(0.32f);
  const size_t tapeFrames = (size_t)(kEchoRate * kEchoSeconds);
  s_tape = (int16_t*)heap_caps_malloc(tapeFrames * sizeof(int16_t),
                                      MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (s_tape) {
    s_echo.init(s_tape, (uint32_t)tapeFrames, (float)kEchoRate);
    s_echo.setFeedback(0.55f);
    s_echo.setLevel(0.5f);
    s_echoBridge.init(&s_echo);
  } else {
    Serial.println("grajek: echo tape alloc FAILED — playing without echo");
  }
  Serial.printf("grajek: free heap after audio init: %u\n",
                (unsigned)esp_get_free_heap_size());

  const BaseType_t ok = xTaskCreatePinnedToCore(
      audioTask, "audio", 8192, nullptr, configMAX_PRIORITIES - 3, nullptr, 0);
  if (ok != pdPASS) {
    i2s_channel_disable(s_tx);
    i2s_del_channel(s_tx);
    s_tx = nullptr;
    if (s_tape) {
      heap_caps_free(s_tape);
      s_tape = nullptr;
    }
    return false;
  }
  s_audioReady.store(true, std::memory_order_release);
  return true;
}

}  // namespace hal
