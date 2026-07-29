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

i2s_chan_handle_t s_tx = nullptr;
ga::Engine* s_engine = nullptr;

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
  for (;;) {
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

}  // namespace

namespace hal {

ga::SympatheticStrings& strings() { return s_strings; }
ga::EchoTape& echo() { return s_echo; }
ga::Reverb& reverb() { return s_reverb; }
bool echoAvailable() { return s_tape != nullptr; }
float audioEnv() { return s_env.load(std::memory_order_relaxed); }

bool audioInit(ga::Engine* engine) {
  s_engine = engine;

  i2s_chan_config_t chanCfg =
      I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
  chanCfg.dma_desc_num = kAudioDmaDescs;
  chanCfg.dma_frame_num = kAudioFrames;
  if (i2s_new_channel(&chanCfg, &s_tx, nullptr) != ESP_OK) return false;

  i2s_std_config_t stdCfg = {};
  stdCfg.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG((uint32_t)engine->sampleRate());
  stdCfg.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                        I2S_SLOT_MODE_STEREO);
  stdCfg.gpio_cfg.mclk = I2S_GPIO_UNUSED;  // no MCLK line — codec runs MCLK=BCLK
  stdCfg.gpio_cfg.bclk = (gpio_num_t)kPinI2sBclk;
  stdCfg.gpio_cfg.ws = (gpio_num_t)kPinI2sWs;
  stdCfg.gpio_cfg.dout = (gpio_num_t)kPinI2sDout;
  stdCfg.gpio_cfg.din = I2S_GPIO_UNUSED;  // mic comes later with the LOOP mode
  stdCfg.gpio_cfg.invert_flags.mclk_inv = false;
  stdCfg.gpio_cfg.invert_flags.bclk_inv = false;
  stdCfg.gpio_cfg.invert_flags.ws_inv = false;
  if (i2s_channel_init_std_mode(s_tx, &stdCfg) != ESP_OK) return false;

  // Start the clocks (DMA plays silence) — a codec in MCLK=BCLK mode needs
  // them running before we configure it.
  if (i2s_channel_enable(s_tx) != ESP_OK) return false;
  vTaskDelay(pdMS_TO_TICKS(20));  // "Codec takes some time to initialize"
  if (!es8311InitPlayback()) return false;

  // --- effect chain ---
  const float sr = engine->sampleRate();
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
  return ok == pdPASS;
}

}  // namespace hal
