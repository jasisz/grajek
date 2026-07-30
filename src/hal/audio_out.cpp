#include "audio_out.h"

#include <M5Unified.h>
#include <driver/i2s_std.h>
#include <esp_heap_caps.h>
#include <esp_rom_gpio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <soc/gpio_sig_map.h>
#include <soc/io_mux_reg.h>

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

// --- the ear (walkie-talkie) ---
// History: we spent a night trying to run the ES8311 ADC as a 48 kHz
// full-duplex slave next to our TX. It cannot work — M5.Mic is an I2S
// master on the same pads (G41/G43/G46, codec without MCLK), and that is
// the only path the factory validated. See git history for the whole
// investigation; the RX-slave experiments were removed with it.
constexpr float kMicRate = 16000.0f;
constexpr int kMicChunk = 256;   // 16 ms per pump chunk
constexpr float kMicMix = 0.8f;  // how much voice enters the tape/strings
constexpr float kAnswerSeconds = 3.0f;  // 96 KB of heap, allocated lazily

std::atomic<bool> s_earOpen{false};        // armed (SPIEW mode)
std::atomic<bool> s_windowActive{false};   // mic owns the bus right now
std::atomic<bool> s_txPaused{false};       // request: audio task, park
std::atomic<bool> s_txParked{false};       // ack from the audio task

// analysis ring for the duet (SPSC: audio task writes, core 1 reads)
constexpr uint32_t kCapSize = 4096;  // power of two, ~256 ms @ 16 kHz
float s_cap[kCapSize];
std::atomic<uint32_t> s_capCount{0};
std::atomic<float> s_earLevel{0.0f};
std::atomic<bool> s_voicePresent{false};
// voice-presence hysteresis; tune on hardware with the serial telemetry
// ("poziom=") — too low and the box answers the fridge
constexpr float kVoiceOn = 0.045f;
constexpr float kVoiceOff = 0.020f;

// the answer buffer: kAnswerSeconds of voice, recorded from the moment a
// voice actually APPEARS (with a 250 ms pre-roll from the analysis ring) —
// not from the window open, or a child who hesitates 3 s would be answered
// with amplified room noise
int16_t* s_answer = nullptr;
std::atomic<int> s_answerLen{0};
std::atomic<bool> s_answerActive{false};
std::atomic<bool> s_answerRestart{false};
std::atomic<uint32_t> s_ansOrigin{0};  // window sample index of s_answer[0]
bool s_ansStarted = false;  // written by the pump (core 0) only

// mic capture stats for diag 'x'
std::atomic<uint32_t> s_micSamples{0};
std::atomic<int> s_micMax{0};

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

// Child-ear ceiling: a hard cap on the final output (~-5 dBFS after a soft
// clip). This instrument is for a kid — no combination of layers may ever
// get loud enough to hurt, on the speaker or on headphones. Kept digital
// (the ES8311 DAC stays at 0 dB for SNR); raise cautiously if ever needed.
constexpr float kEarCeiling = 0.55f;

std::atomic<float> s_env{0.0f};

bool es8311Write(uint8_t reg, uint8_t val) {
  return M5.In_I2C.writeRegister8(hal::kEs8311Addr, reg, val, 400000);
}

uint8_t es8311Read(uint8_t reg) {
  return M5.In_I2C.readRegister8(hal::kEs8311Addr, reg, 400000);
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

// One mic chunk into the analysis ring + level envelope + answer buffer.
// Runs on core 0 (audio task) while the window is open.
void pumpChunk(const int16_t* in, int n) {
  uint32_t c = s_capCount.load(std::memory_order_relaxed);
  float lvl = s_earLevel.load(std::memory_order_relaxed);
  bool present = s_voicePresent.load(std::memory_order_relaxed);
  int mx = s_micMax.load(std::memory_order_relaxed);
  int alen = s_answerLen.load(std::memory_order_relaxed);
  const int aCap = (int)(kMicRate * kAnswerSeconds);
  for (int i = 0; i < n; ++i) {
    const int raw = in[i];
    const int ar = raw < 0 ? -raw : raw;
    if (ar > mx) mx = ar;
    const float v = (float)raw * (1.0f / 32768.0f);
    s_cap[(c++) & (kCapSize - 1)] = v;
    const float a = fabsf(v);
    // same time constants as the old 48 kHz path, rescaled to 16 kHz
    lvl += (a > lvl ? 0.03f : 0.0006f) * (a - lvl);
    if (lvl > kVoiceOn) present = true;
    else if (lvl < kVoiceOff) present = false;
    if (s_answer && !s_ansStarted && present) {
      // the voice just appeared: start the answer here, with the attack
      // rescued from the analysis ring (250 ms pre-roll)
      s_ansStarted = true;
      const uint32_t pre = c > 4000u ? 4000u : c;
      s_ansOrigin.store(c - pre, std::memory_order_relaxed);
      for (uint32_t k = c - pre; k < c && alen < aCap; ++k)
        s_answer[alen++] = (int16_t)(s_cap[k & (kCapSize - 1)] * 32767.0f);
    }
    if (s_answer && s_ansStarted && alen < aCap) s_answer[alen++] = (int16_t)raw;
  }
  s_capCount.store(c, std::memory_order_release);
  s_earLevel.store(lvl, std::memory_order_relaxed);
  s_voicePresent.store(present, std::memory_order_relaxed);
  s_micMax.store(mx, std::memory_order_relaxed);
  s_answerLen.store(alen, std::memory_order_relaxed);
  s_micSamples.fetch_add((uint32_t)n, std::memory_order_relaxed);
}

void audioTask(void*) {
  static float fbuf[hal::kAudioFrames];
  static int16_t mono[hal::kAudioFrames];
  static int16_t stereo[hal::kAudioFrames * 2];
  static int16_t micBuf[kMicChunk];
  float envLocal = 0.0f;
  float ansPos = 0.0f;    // answer playhead, in mic samples (x3 upsample)
  float parkGain = 1.0f;  // ~8 ms fade before the bus is handed to the mic
  for (;;) {
    const bool pauseReq = s_txPaused.load(std::memory_order_relaxed);
    if (pauseReq && parkGain <= 0.0f) {
      // WINDOW: the mic owns the bus. The engine keeps draining its event
      // queue (ambient ticks on), the effects stay frozen mid-thought —
      // the tape must not record the silence over the memories.
      s_txParked.store(true, std::memory_order_release);
      s_engine->process(fbuf, hal::kAudioFrames);
      if (s_windowActive.load(std::memory_order_acquire)) {
        if (M5.Mic.record(micBuf, kMicChunk)) {
          while (M5.Mic.isRecording() &&
                 s_windowActive.load(std::memory_order_relaxed))
            vTaskDelay(1);
          if (s_windowActive.load(std::memory_order_relaxed))
            pumpChunk(micBuf, kMicChunk);
        } else {
          vTaskDelay(1);
        }
      } else {
        vTaskDelay(pdMS_TO_TICKS(2));
      }
      continue;
    }

    // epoch ack: the park flag is OURS to clear — if txResume cleared it,
    // a stale ack could survive a fast close/open cycle and let core 1
    // yank the channel from under a live i2s_channel_write
    s_txParked.store(false, std::memory_order_relaxed);

    s_engine->process(fbuf, hal::kAudioFrames);
    // publish the dry-synth envelope for the weather system
    for (int i = 0; i < hal::kAudioFrames; ++i) {
      const float a = fabsf(fbuf[i]);
      envLocal += (a > envLocal ? 0.004f : 0.00001f) * (a - envLocal);
    }
    s_env.store(envLocal, std::memory_order_relaxed);

    // THE ANSWER: the captured voice re-enters the chain exactly where the
    // synth does — the tape ages it, the strings halo it, the reverb gives
    // it the room. Linear 3x upsample, 10 ms fades on both ends.
    if (s_answerActive.load(std::memory_order_relaxed)) {
      if (s_answerRestart.exchange(false, std::memory_order_relaxed))
        ansPos = 0.0f;
      const int len = s_answerLen.load(std::memory_order_relaxed);
      for (int i = 0; i < hal::kAudioFrames; ++i) {
        const int idx = (int)ansPos;
        if (idx + 1 >= len) {
          s_answerActive.store(false, std::memory_order_relaxed);
          break;
        }
        const float frac = ansPos - (float)idx;
        const float v = ((1.0f - frac) * (float)s_answer[idx] +
                         frac * (float)s_answer[idx + 1]) *
                        (1.0f / 32768.0f);
        const float fade =
            fminf(1.0f, fminf(ansPos, (float)(len - 1) - ansPos) *
                            (1.0f / 160.0f));
        fbuf[i] += v * kMicMix * fade;
        ansPos += 1.0f / 3.0f;
      }
    }

    s_strings.process(fbuf, fbuf, hal::kAudioFrames);
    if (s_tape) s_echoBridge.process(fbuf, hal::kAudioFrames);
    s_reverb.process(fbuf, hal::kAudioFrames);

    // park fade: a requested listening window ramps the tail down over
    // four blocks instead of letting the DMA click it off mid-sample
    if (pauseReq) {
      const float g1 = parkGain - 0.25f < 0.0f ? 0.0f : parkGain - 0.25f;
      for (int i = 0; i < hal::kAudioFrames; ++i)
        fbuf[i] *= parkGain +
                   (g1 - parkGain) * (float)i * (1.0f / hal::kAudioFrames);
      parkGain = g1;
    } else {
      parkGain = 1.0f;
    }

    // speaker voicing, then the glue clip + child-ear ceiling
    for (int i = 0; i < hal::kAudioFrames; ++i) {
      const float v = s_spkShelf.process(s_spkHp.process(fbuf[i]));
      fbuf[i] = ga::softClip(v) * kEarCeiling;
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

// Give the bus back to the speaker: pads to I2S1, codec re-latched at dead
// clocks (the ES8311 only latches its config on a clean clock start), then
// a clean TX start. Core 1 only — all I2C lives on core 1.
void txResume() {
  gpio_set_direction((gpio_num_t)hal::kPinI2sBclk, GPIO_MODE_OUTPUT);
  gpio_set_direction((gpio_num_t)hal::kPinI2sWs, GPIO_MODE_OUTPUT);
  gpio_set_direction((gpio_num_t)hal::kPinI2sDout, GPIO_MODE_OUTPUT);
  esp_rom_gpio_connect_out_signal(hal::kPinI2sBclk, I2S1O_BCK_OUT_IDX, false,
                                  false);
  esp_rom_gpio_connect_out_signal(hal::kPinI2sWs, I2S1O_WS_OUT_IDX, false,
                                  false);
  esp_rom_gpio_connect_out_signal(hal::kPinI2sDout, I2S1O_SD_OUT_IDX, false,
                                  false);
  es8311Write(0x00, 0x1F);  // full reset — M5.Mic.end() left it powered down
  vTaskDelay(pdMS_TO_TICKS(10));
  es8311Write(0x00, 0x00);
  es8311InitPlayback();
  vTaskDelay(pdMS_TO_TICKS(20));
  i2s_channel_enable(s_tx);
  // only release the pause — the audio task clears its own park ack the
  // moment it re-enters the live path (epoch ack, no stale-flag race)
  s_txPaused.store(false, std::memory_order_relaxed);
}

}  // namespace

namespace hal {

ga::SympatheticStrings& strings() { return s_strings; }
ga::EchoTape& echo() { return s_echo; }
ga::Reverb& reverb() { return s_reverb; }
bool echoAvailable() { return s_tape != nullptr; }
float audioEnv() { return s_env.load(std::memory_order_relaxed); }

bool earAvailable() { return s_tx != nullptr && M5.Mic.isEnabled(); }
bool earIsOpen() { return s_earOpen.load(std::memory_order_relaxed); }
bool earWindowActive() {
  return s_windowActive.load(std::memory_order_relaxed);
}
float earSampleRate() { return kMicRate; }
float earLevel() { return s_earLevel.load(std::memory_order_relaxed); }
bool earVoicePresent() {
  return s_voicePresent.load(std::memory_order_relaxed);
}

bool earWindowOpen() {
  if (!s_earOpen.load(std::memory_order_relaxed) || !s_tx) return false;
  if (s_windowActive.load(std::memory_order_relaxed)) return true;
  if (!M5.Mic.isEnabled()) return false;
  s_answerActive.store(false, std::memory_order_relaxed);
  s_txPaused.store(true, std::memory_order_relaxed);
  // the audio task may sit inside a blocking DMA write — wait for the park
  for (int i = 0; i < 100 && !s_txParked.load(std::memory_order_acquire); ++i)
    vTaskDelay(1);
  i2s_channel_disable(s_tx);
  auto mc = M5.Mic.config();
  mc.sample_rate = (uint32_t)kMicRate;
  mc.task_pinned_core = 0;  // the audio task idles there during the window
  M5.Mic.config(mc);
  // fresh window: counters and gates start clean (a stale gate once kept
  // the old duplex duck half-open across windows)
  s_capCount.store(0, std::memory_order_relaxed);
  s_earLevel.store(0.0f, std::memory_order_relaxed);
  s_voicePresent.store(false, std::memory_order_relaxed);
  s_answerLen.store(0, std::memory_order_relaxed);
  s_ansOrigin.store(0, std::memory_order_relaxed);
  s_ansStarted = false;  // before the windowActive release-store: safe
  if (!M5.Mic.begin()) {  // codec ADC config happens in the M5Unified callback
    Serial.println("grajek: ucho: M5.Mic.begin FAILED");
    txResume();
    return false;
  }
  s_windowActive.store(true, std::memory_order_release);
  Serial.println("grajek: ucho: okno OTWARTE (mikrofon ma magistrale)");
  return true;
}

void earWindowClose() {
  if (!s_windowActive.load(std::memory_order_relaxed)) return;
  s_windowActive.store(false, std::memory_order_relaxed);
  vTaskDelay(pdMS_TO_TICKS(4));  // let the pump abandon its chunk
  M5.Mic.end();  // stops the mic task, powers the codec down (callback)
  txResume();
  Serial.printf("grajek: ucho: okno zamkniete (probki=%u max=%d)\n",
                (unsigned)s_micSamples.load(), s_micMax.load());
}

void earSetOpen(bool open) {
  if (open) {
    if (!s_answer) {
      s_answer = (int16_t*)heap_caps_malloc(
          (size_t)(kMicRate * kAnswerSeconds) * sizeof(int16_t),
          MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
      if (!s_answer)
        Serial.println(
            "grajek: answer buffer alloc FAILED — listening without echo");
    }
    s_earLevel.store(0.0f, std::memory_order_relaxed);
    s_voicePresent.store(false, std::memory_order_relaxed);
  } else {
    if (s_windowActive.load(std::memory_order_relaxed)) earWindowClose();
    s_answerActive.store(false, std::memory_order_relaxed);
  }
  s_earOpen.store(open, std::memory_order_relaxed);
}

uint32_t earCaptured() { return s_capCount.load(std::memory_order_acquire); }

void earWindow(float* out, int n) {
  const uint32_t c = s_capCount.load(std::memory_order_acquire);
  const uint32_t start = c - (uint32_t)n;
  for (int i = 0; i < n; ++i)
    out[i] = s_cap[(start + (uint32_t)i) & (kCapSize - 1)];
}

bool earAnswerStart() {
  if (!s_answer || s_answerLen.load(std::memory_order_relaxed) < 800)
    return false;  // < 50 ms of voice is a click, not an answer
  if (s_windowActive.load(std::memory_order_relaxed)) return false;
  s_answerRestart.store(true, std::memory_order_relaxed);
  s_answerActive.store(true, std::memory_order_relaxed);
  return true;
}

bool earAnswerActive() {
  return s_answerActive.load(std::memory_order_relaxed);
}

uint32_t earAnswerOrigin() {
  return s_ansOrigin.load(std::memory_order_relaxed);
}

void audioDiagWrite(uint8_t reg, uint8_t val) {
  const bool ok = es8311Write(reg, val);
  Serial.printf("grajek: diag W %02X=%02X -> %s (odczyt: %02X)\n", reg, val,
                ok ? "OK" : "I2C FAIL", es8311Read(reg));
}

void audioDiag(char cmd) {
  switch (cmd) {
    case 'r': {
      Serial.println("grajek: ES8311 rejestry 0x00..0x45:");
      for (uint8_t reg = 0; reg <= 0x45; ++reg) {
        const uint8_t v = M5.In_I2C.readRegister8(kEs8311Addr, reg, 400000);
        Serial.printf("  %02X=%02X%s", reg, v, (reg & 7) == 7 ? "\n" : "");
      }
      Serial.println();
      break;
    }
    case 'g':
      Serial.printf("grajek: diag PGA maks (0x14=0x17) -> %s\n",
                    es8311Write(0x14, 0x17) ? "OK" : "I2C FAIL");
      break;
    case 'x':
      Serial.printf("grajek: diag mic probki=%u max=%d ans=%d heap=%u\n",
                    (unsigned)s_micSamples.load(), s_micMax.load(),
                    (int)s_answerLen.load(),
                    (unsigned)esp_get_free_heap_size());
      s_micSamples.store(0);
      s_micMax.store(0);
      break;
    case 'p': {
      // surowy podgląd pada G46: liczymy jedynki z szybkiego próbkowania —
      // linia z danymi I2S da mieszankę, martwa linia da 0 albo maks
      PIN_INPUT_ENABLE(GPIO_PIN_MUX_REG[kPinI2sDin]);
      int ones = 0;
      constexpr int kN = 20000;
      for (int i = 0; i < kN; ++i)
        ones += gpio_get_level((gpio_num_t)kPinI2sDin);
      Serial.printf("grajek: diag pad G46: %d/%d jedynek\n", ones, kN);
      break;
    }
    case 'l': {
      // wewnętrzny loopback kodeka ADC->DAC (rejestr 0x44): jak głośnik
      // zapiszczy sprzężeniem, to mikrofon+ADC żyją
      static bool loop = false;
      loop = !loop;
      Serial.printf("grajek: diag loopback ADC->DAC %s -> %s\n",
                    loop ? "ON" : "off",
                    es8311Write(0x44, loop ? 0x08 : 0x00) ? "OK" : "I2C FAIL");
      break;
    }
    case 'k': {
      // RYTUAŁ RELATCH: kodek łapie konfigurację tylko przy czystym starcie
      // zegarów — zatrzymaj TX, zrestartuj kodek, wystartuj czysto.
      s_txPaused.store(true);
      for (int i = 0; i < 100 && !s_txParked.load(); ++i) vTaskDelay(1);
      i2s_channel_disable(s_tx);
      txResume();
      Serial.println("grajek: diag relatch — zegary zrestartowane");
      break;
    }
    case 'e': {
      // ręczne okno słuchania: pierwszy raz otwiera, drugi zamyka,
      // wypisuje statystyki i puszcza odpowiedź (pełny cykl krótkofalówki)
      if (!earWindowActive()) {
        earSetOpen(true);
        Serial.printf("grajek: diag okno -> %s\n",
                      earWindowOpen() ? "OTWARTE (spiewaj)" : "FAIL");
      } else {
        earWindowClose();
        Serial.printf("grajek: diag okno zamkniete: probki=%u max=%d\n",
                      (unsigned)s_micSamples.load(), s_micMax.load());
        Serial.printf("grajek: diag odpowiedz -> %s\n",
                      earAnswerStart() ? "GRA" : "brak nagrania");
        earSetOpen(false);
      }
      break;
    }
    case 'M': {
      // automat: 700 ms okna, zamknięcie, odpowiedź — dowód całego cyklu
      // jedną literą (blokuje loop() na chwilę; audio i tak milczy w oknie)
      earSetOpen(true);
      if (!earWindowOpen()) {
        Serial.println("grajek: diag M: okno FAIL");
        earSetOpen(false);
        break;
      }
      Serial.println("grajek: diag M: nagrywam 700 ms...");
      vTaskDelay(pdMS_TO_TICKS(700));
      earWindowClose();
      Serial.printf("grajek: diag M: probki=%u max=%d odpowiedz=%s\n",
                    (unsigned)s_micSamples.load(), s_micMax.load(),
                    earAnswerStart() ? "GRA" : "cisza");
      earSetOpen(false);
      break;
    }
    default:
      break;
  }
}

bool audioInit(ga::Engine* engine) {
  s_engine = engine;

  // TX only: I2S1 master, the verified speaker path. The ear does not get
  // an RX channel at all — it borrows the whole bus in windows (M5.Mic).
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
  if (i2s_new_channel(&chanCfg, &s_tx, nullptr) != ESP_OK) return false;
  if (i2s_channel_init_std_mode(s_tx, &txCfg) != ESP_OK) return false;

  // Codec at DEAD clocks (like M5Unified) — the ES8311 only latches its
  // configuration on a clean clock start.
  if (!es8311InitPlayback()) return false;
  vTaskDelay(pdMS_TO_TICKS(20));

  if (i2s_channel_enable(s_tx) != ESP_OK) return false;

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
  Serial.printf("grajek: ucho %s (krotkofalowka M5.Mic)\n",
                M5.Mic.isEnabled() ? "GOTOWE" : "BRAK (internal_mic?)");

  const BaseType_t ok = xTaskCreatePinnedToCore(
      audioTask, "audio", 8192, nullptr, configMAX_PRIORITIES - 3, nullptr, 0);
  return ok == pdPASS;
}

}  // namespace hal
