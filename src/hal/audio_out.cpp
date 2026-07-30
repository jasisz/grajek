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

i2s_chan_handle_t s_tx = nullptr;
i2s_chan_handle_t s_rx = nullptr;  // ucho: nullptr gdy full-duplex nie wstał
ga::Engine* s_engine = nullptr;

// --- ucho ---
std::atomic<bool> s_earOpen{false};
std::atomic<bool> s_earDuet{false};
// PGA kodeka stoi na oficjalnym minimum — brakujący gain nadrabiamy cyfrowo;
// do strojenia uchem na sprzęcie (za cicho -> podnieś, przester -> obniż)
constexpr float kMicGain = 4.0f;
constexpr float kMicMix = 0.8f;  // ile głosu wchodzi w taśmę/struny
// pierścień analizy dla duetu (SPSC: audio task pisze, rdzeń 1 czyta)
constexpr uint32_t kCapSize = 4096;  // potęga dwójki, ~85 ms @ 48 kHz
float s_cap[kCapSize];
std::atomic<uint32_t> s_capCount{0};
std::atomic<float> s_earLevel{0.0f};  // obwiednia głosu (po kMicGain)
std::atomic<bool> s_voicePresent{false};
// sondy diagnostyczne strumienia RX (niezależne od otwarcia ucha)
std::atomic<uint32_t> s_rxBytes{0};
std::atomic<int> s_rxMaxL{0};
std::atomic<int> s_rxMaxR{0};
// histereza obecności głosu; przy pompowaniu (duck faluje bez śpiewu,
// bo mikrofon słyszy głośnik) podnieś kVoiceOn — telemetria „poziom="
// na serialu mówi, gdzie leżą realne wartości
constexpr float kVoiceOn = 0.045f;
constexpr float kVoiceOff = 0.020f;

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

// Child-ear ceiling: a hard cap on the final output (~-5 dBFS after a soft
// clip). This instrument is for a kid — no combination of layers may ever
// get loud enough to hurt, on the speaker or on headphones. Kept digital
// (the ES8311 DAC stays at 0 dB for SNR); raise cautiously if ever needed.
constexpr float kEarCeiling = 0.55f;

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

uint8_t es8311Read(uint8_t reg) {
  return M5.In_I2C.readRegister8(hal::kEs8311Addr, reg, 400000);
}

// Wierna transkrypcja es8311_init() + microphone_config() z espressif/esp-bsp
// (components/es8311/es8311.c) dla: MCLK z linii BCLK, 48 kHz, 16/16 bit,
// slave, ADC+DAC naraz. Wiersz tabeli współczynników dla mclk=1536000/48k:
// pre_div=1 pre_multi=3 adc_div=1 dac_div=1 fs=0 lrck=00FF bclk_div=4 osr=0x10.
// Moje wcześniejsze "fuzje" oficjalnych sekwencji M5 zostawiały ASDOUT
// w stanie wysokim (ADC nie ruszał) — ta wersja jest z działającego
// sterownika pełnodupleksowego.
bool es8311InitDuplex() {
  bool ok = true;
  ok &= es8311Write(0x00, 0x1F);  // pełny reset
  vTaskDelay(pdMS_TO_TICKS(20));
  ok &= es8311Write(0x00, 0x00);
  ok &= es8311Write(0x00, 0x80);  // CSM power on
  ok &= es8311Write(0x01, 0xBF);  // wszystkie zegary + MCLK z BCLK
  ok &= es8311Write(0x06, es8311Read(0x06) & (uint8_t)~0x20);  // SCLK nieodwr.
  ok &= es8311Write(0x02, (es8311Read(0x02) & 0x07) | 0x18);   // pre_div/multi
  ok &= es8311Write(0x03, 0x10);  // fs_mode + adc_osr
  ok &= es8311Write(0x04, 0x10);  // dac_osr
  ok &= es8311Write(0x05, 0x00);  // adc_div / dac_div
  ok &= es8311Write(0x06, (es8311Read(0x06) & 0xE0) | 0x03);   // bclk_div=4
  ok &= es8311Write(0x07, es8311Read(0x07) & 0xC0);            // lrck_h
  ok &= es8311Write(0x08, 0xFF);                               // lrck_l
  ok &= es8311Write(0x00, es8311Read(0x00) & 0xBF);  // tryb slave
  ok &= es8311Write(0x09, 0x0C);  // SDP IN:  I2S, 16 bit
  ok &= es8311Write(0x0A, 0x0C);  // SDP OUT: I2S, 16 bit
  ok &= es8311Write(0x0D, 0x01);  // power up analog
  ok &= es8311Write(0x0E, 0x02);  // analog PGA + modulator ADC
  ok &= es8311Write(0x12, 0x00);  // power up DAC
  ok &= es8311Write(0x13, 0x10);  // wyjście na HP drive (NS4150B)
  ok &= es8311Write(0x1C, 0x6A);  // ADC: bypass EQ + kasowanie DC
  ok &= es8311Write(0x37, 0x08);  // DAC: bypass EQ
  ok &= es8311Write(0x14, 0x1A);  // mikrofon analogowy + wzmocnienie PGA
  ok &= es8311Write(0x17, 0xC8);  // gain ADC
  ok &= es8311Write(0x32, 0xBF);  // głośność DAC ~0 dB
  return ok;
}

void audioTask(void*) {
  static float fbuf[hal::kAudioFrames];
  static int16_t mono[hal::kAudioFrames];
  static int16_t stereo[hal::kAudioFrames * 2];
  static int16_t micStereo[hal::kAudioFrames * 2];
  float envLocal = 0.0f;
  float duck = 1.0f;
  for (;;) {
    s_engine->process(fbuf, hal::kAudioFrames);
    // publish the dry-synth envelope for the weather system
    for (int i = 0; i < hal::kAudioFrames; ++i) {
      const float a = fabsf(fbuf[i]);
      envLocal += (a > envLocal ? 0.004f : 0.00001f) * (a - envLocal);
    }
    s_env.store(envLocal, std::memory_order_relaxed);

    // UCHO: RX czytamy zawsze (DMA nie może się zapchać), ale głos żyje
    // tylko przy otwartym uchu — miesza się w łańcuch dokładnie tam, gdzie
    // synth (taśma go starzy, struny otulają) i ląduje w oknie analizy duetu
    if (s_rx) {
      size_t got = 0;
      i2s_channel_read(s_rx, micStereo, sizeof(micStereo), &got, 0);
      const int micN = (int)(got / (2 * sizeof(int16_t)));
      if (got > 0) {
        s_rxBytes.fetch_add((uint32_t)got, std::memory_order_relaxed);
        int mL = s_rxMaxL.load(std::memory_order_relaxed);
        int mR = s_rxMaxR.load(std::memory_order_relaxed);
        for (int i = 0; i < micN; ++i) {
          const int aL = abs((int)micStereo[2 * i]);
          const int aR = abs((int)micStereo[2 * i + 1]);
          if (aL > mL) mL = aL;
          if (aR > mR) mR = aR;
        }
        s_rxMaxL.store(mL, std::memory_order_relaxed);
        s_rxMaxR.store(mR, std::memory_order_relaxed);
      }
      if (s_earOpen.load(std::memory_order_relaxed) && micN > 0) {
        // KRÓTKOFALÓWKA STEROWANA GŁOSEM: mikser otwiera się tylko, gdy
        // słychać śpiew (histereza na obwiedni). Śpiewasz -> głos wchodzi
        // w taśmę, a wyjście przycicha; milkniesz -> mikser się domyka,
        // duck puszcza i pudełko ODPOWIADA nagranym głosem. W obu stanach
        // pętla mik->głośnik->mik jest zagłodzona.
        static float gate = 0.0f;
        uint32_t c = s_capCount.load(std::memory_order_relaxed);
        float lvl = s_earLevel.load(std::memory_order_relaxed);
        bool present = s_voicePresent.load(std::memory_order_relaxed);
        for (int i = 0; i < micN; ++i) {
          const float v =
              (float)micStereo[2 * i] * (kMicGain / 32768.0f);  // kanał L
          s_cap[(c++) & (kCapSize - 1)] = v;
          const float a = fabsf(v);
          lvl += (a > lvl ? 0.01f : 0.0002f) * (a - lvl);
          if (lvl > kVoiceOn) present = true;
          else if (lvl < kVoiceOff) present = false;
          gate += (present ? 0.003f : -0.0006f);  // atak ~7 ms, opad ~35 ms
          if (gate > 1.0f) gate = 1.0f;
          if (gate < 0.0f) gate = 0.0f;
          if (i < hal::kAudioFrames) fbuf[i] += v * kMicMix * gate;
        }
        s_capCount.store(c, std::memory_order_release);
        s_earLevel.store(lvl, std::memory_order_relaxed);
        s_voicePresent.store(present, std::memory_order_relaxed);
      }
    }

    s_strings.process(fbuf, fbuf, hal::kAudioFrames);
    if (s_tape) s_echoBridge.process(fbuf, hal::kAudioFrames);
    s_reverb.process(fbuf, hal::kAudioFrames);

    // duck tylko PODCZAS śpiewu (nie przez cały tryb) — po zamilknięciu
    // wyjście wraca i odpowiedź z taśmy naprawdę wybrzmiewa; gdy duet
    // nuci, duck lżejszy — towarzysza ma być słychać (wciąż ~-9 dB)
    const float duckTarget =
        (s_earOpen.load(std::memory_order_relaxed) &&
         s_voicePresent.load(std::memory_order_relaxed))
            ? (s_earDuet.load(std::memory_order_relaxed) ? 0.35f : 0.15f)
            : 1.0f;
    for (int i = 0; i < hal::kAudioFrames; ++i) {
      duck += 0.0008f * (duckTarget - duck);  // ~30 ms rampy, bez trzasków
      fbuf[i] *= duck;
    }

    // final glue clip + the child-ear ceiling — nothing gets past this
    for (int i = 0; i < hal::kAudioFrames; ++i)
      fbuf[i] = ga::softClip(fbuf[i]) * kEarCeiling;

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

// EKSPERYMENT: 1 = tor fabryczny 1:1 (sam RX, bez TX — jednostka RX jest
// masterem zegarów jak w M5.Mic; 16 kHz mono, kodek mic-only, PGA max).
// 2 = PODSŁUCH: jak 1, ale NIE DOTYKAMY rejestrów kodeka — kodek trzyma
// stan zostawiony przez fabryczny firmware (flash nie resetuje ES8311),
// zrzut 'r' pokaże PRAWDZIWĄ działającą konfigurację.
// Głośnik milczy; sondy 'x' mierzą szum pokoju. Po eksperymencie wrócić na 0!
#define EAR_EXPERIMENT_RX_ONLY 0

namespace hal {

ga::SympatheticStrings& strings() { return s_strings; }
ga::EchoTape& echo() { return s_echo; }
ga::Reverb& reverb() { return s_reverb; }
bool echoAvailable() { return s_tape != nullptr; }
float audioEnv() { return s_env.load(std::memory_order_relaxed); }

bool earAvailable() { return s_rx != nullptr; }
bool earIsOpen() { return s_earOpen.load(std::memory_order_relaxed); }
float earLevel() { return s_earLevel.load(std::memory_order_relaxed); }

void earSetOpen(bool open) {
  if (open && !s_rx) return;  // głuche ucho nie może duckować muzyki
  if (open) {
    s_capCount.store(0, std::memory_order_relaxed);  // świeże okno
    s_earLevel.store(0.0f, std::memory_order_relaxed);
    s_voicePresent.store(false, std::memory_order_relaxed);
  }
  s_earOpen.store(open, std::memory_order_relaxed);
  if (!open) s_earDuet.store(false, std::memory_order_relaxed);
}

void earSetDuet(bool on) { s_earDuet.store(on, std::memory_order_relaxed); }

uint32_t earCaptured() { return s_capCount.load(std::memory_order_acquire); }

void earWindow(float* out, int n) {
  const uint32_t c = s_capCount.load(std::memory_order_acquire);
  const uint32_t start = c - (uint32_t)n;
  for (int i = 0; i < n; ++i)
    out[i] = s_cap[(start + (uint32_t)i) & (kCapSize - 1)];
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
        const uint8_t v =
            M5.In_I2C.readRegister8(kEs8311Addr, reg, 400000);
        Serial.printf("  %02X=%02X%s", reg, v, (reg & 7) == 7 ? "\n" : "");
      }
      Serial.println();
      break;
    }
    case 'd':
      Serial.printf("grajek: diag kodek DUPLEX -> %s\n",
                    es8311InitDuplex() ? "OK" : "I2C FAIL");
      break;
    case 'm': {
      // oficjalna sekwencja mic-only z M5Unified — głośnik zamilknie;
      // to test, czy ADC w ogóle umie żyć na tym I2S RX
      static constexpr uint8_t seq[][2] = {
          {0x00, 0x80}, {0x01, 0xBA}, {0x02, 0x18}, {0x0D, 0x01},
          {0x0E, 0x02}, {0x14, 0x10}, {0x17, 0xBF}, {0x1C, 0x6A},
      };
      bool ok = true;
      for (auto& rv : seq) ok = ok && es8311Write(rv[0], rv[1]);
      Serial.printf("grajek: diag kodek MIC-ONLY -> %s\n",
                    ok ? "OK" : "I2C FAIL");
      break;
    }
    case 'a': {
      // duplex rozszerzony o rejestry, które pełny sterownik ESP-ADF
      // ustawia jawnie (reset-puls, OSR, format SDP 16-bit I2S) —
      // wartości z es8311.c, do weryfikacji uchem/rms
      es8311Write(0x00, 0x1F);  // pełny reset
      vTaskDelay(pdMS_TO_TICKS(20));
      es8311Write(0x00, 0x00);
      vTaskDelay(pdMS_TO_TICKS(20));
      static constexpr uint8_t seq[][2] = {
          {0x00, 0x80}, {0x01, 0xBF}, {0x02, 0x18}, {0x03, 0x10},
          {0x04, 0x10}, {0x05, 0x00}, {0x09, 0x0C}, {0x0A, 0x0C},
          {0x0B, 0x00}, {0x0C, 0x00}, {0x0D, 0x01}, {0x0E, 0x02},
          {0x12, 0x00}, {0x13, 0x10}, {0x14, 0x10}, {0x15, 0x40},
          {0x16, 0x24}, {0x17, 0xBF}, {0x1B, 0x0A}, {0x1C, 0x6A},
          {0x32, 0xBF}, {0x37, 0x08},
      };
      bool ok = true;
      for (auto& rv : seq) ok = ok && es8311Write(rv[0], rv[1]);
      Serial.printf("grajek: diag kodek DUPLEX-ADF -> %s\n",
                    ok ? "OK" : "I2C FAIL");
      break;
    }
    case 'g':
      Serial.printf("grajek: diag PGA maks (0x14=0x17) -> %s\n",
                    es8311Write(0x14, 0x17) ? "OK" : "I2C FAIL");
      break;
    case 'x': {
      // sondy RX: ile bajtów dopłynęło i jaka maksymalna amplituda per slot
      Serial.printf("grajek: diag RX bajty=%u maxL=%d maxR=%d\n",
                    (unsigned)s_rxBytes.load(), s_rxMaxL.load(),
                    s_rxMaxR.load());
      s_rxMaxL.store(0);
      s_rxMaxR.store(0);
      break;
    }
    case 'p': {
      // surowy podgląd pada G46: liczymy jedynki z szybkiego próbkowania —
      // linia z danymi I2S da mieszankę, martwa linia da 0 albo maks
      PIN_INPUT_ENABLE(GPIO_PIN_MUX_REG[kPinI2sDin]);
      int ones = 0;
      constexpr int kN = 20000;
      for (int i = 0; i < kN; ++i) ones += gpio_get_level((gpio_num_t)kPinI2sDin);
      Serial.printf("grajek: diag pad G46: %d/%d jedynek\n", ones, kN);
      break;
    }
    case 'w':
      // pętla po matrycy GPIO: wejście RX <- nasza linia TX (pin 42);
      // RX powinien czytać dokładnie to, co gramy — rozstrzyga, czy
      // jednostka RX w ogóle próbkuje
      PIN_INPUT_ENABLE(GPIO_PIN_MUX_REG[kPinI2sDout]);
      esp_rom_gpio_connect_in_signal(kPinI2sDout, I2S1I_SD_IN_IDX, false);
      Serial.println("grajek: diag RX <- DOUT (petla wlasnego TX)");
      break;
    case 'q':
      esp_rom_gpio_connect_in_signal(kPinI2sDin, I2S1I_SD_IN_IDX, false);
      Serial.println("grajek: diag RX <- G46 (mikrofon, normalnie)");
      break;
    case 'l': {
      // wewnętrzny loopback kodeka ADC->DAC (rejestr 0x44): jak głośnik
      // zapiszczy sprzężeniem, to mikrofon+ADC żyją i winny jest I2S RX
      static bool loop = false;
      loop = !loop;
      Serial.printf("grajek: diag loopback ADC->DAC %s -> %s\n",
                    loop ? "ON" : "off",
                    es8311Write(0x44, loop ? 0x08 : 0x00) ? "OK" : "I2C FAIL");
      break;
    }
    case 'e': {
      const bool open = !earIsOpen();
      earSetOpen(open);
      Serial.printf("grajek: diag ucho %s (rx=%s)\n",
                    earIsOpen() ? "OTWARTE" : "zamkniete",
                    s_rx ? "jest" : "BRAK");
      break;
    }
    case 'k': {
      // RYTUAŁ RELATCH: kodek łapie konfigurację tylko przy czystym starcie
      // zegarów — zatrzymaj I2S, zrestartuj CSM, wystartuj, napraw routing.
      // Rejestry ustawione W-kami przed 'k' zaczynają działać dopiero po nim.
      i2s_channel_disable(s_tx);
      if (s_rx) i2s_channel_disable(s_rx);
      vTaskDelay(pdMS_TO_TICKS(30));
      es8311Write(0x00, 0x00);
      es8311Write(0x00, 0x80);
      vTaskDelay(pdMS_TO_TICKS(20));
      i2s_channel_enable(s_tx);
      if (s_rx) {
        i2s_channel_enable(s_rx);
        gpio_set_direction((gpio_num_t)kPinI2sBclk, GPIO_MODE_INPUT_OUTPUT);
        gpio_set_direction((gpio_num_t)kPinI2sWs, GPIO_MODE_INPUT_OUTPUT);
        esp_rom_gpio_connect_out_signal(kPinI2sBclk, I2S0I_BCK_OUT_IDX, false,
                                        false);
        esp_rom_gpio_connect_out_signal(kPinI2sWs, I2S0I_WS_OUT_IDX, false,
                                        false);
        esp_rom_gpio_connect_in_signal(kPinI2sBclk, I2S1O_BCK_IN_IDX, false);
        esp_rom_gpio_connect_in_signal(kPinI2sWs, I2S1O_WS_IN_IDX, false);
        PIN_INPUT_ENABLE(GPIO_PIN_MUX_REG[kPinI2sDin]);
        esp_rom_gpio_connect_in_signal(kPinI2sDin, I2S0I_SD_IN_IDX, false);
      }
      Serial.println("grajek: diag relatch — zegary zrestartowane");
      break;
    }
    default:
      break;
  }
}

bool audioInit(ga::Engine* engine) {
  s_engine = engine;

#if EAR_EXPERIMENT_RX_ONLY
  // --- tor fabryczny 1:1: sam RX, master, 16 kHz, mono LEFT, 16-bit ---
  {
    // I2S_NUM_0 jak w fabrycznym torze mikrofonu (M5Unified zostawia
    // mikrofonowi domyślny port 0; my dotąd wszystko robiliśmy na 1)
    i2s_chan_config_t rxChanCfg =
        I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    if (i2s_new_channel(&rxChanCfg, nullptr, &s_rx) != ESP_OK) return false;
    i2s_std_config_t rxCfg = {};
    rxCfg.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(16000);
    rxCfg.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
        I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO);
    rxCfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;
    rxCfg.gpio_cfg.mclk = I2S_GPIO_UNUSED;
    rxCfg.gpio_cfg.bclk = (gpio_num_t)kPinI2sBclk;
    rxCfg.gpio_cfg.ws = (gpio_num_t)kPinI2sWs;
    rxCfg.gpio_cfg.dout = I2S_GPIO_UNUSED;
    rxCfg.gpio_cfg.din = (gpio_num_t)kPinI2sDin;
    if (i2s_channel_init_std_mode(s_rx, &rxCfg) != ESP_OK) return false;
#if EAR_EXPERIMENT_RX_ONLY == 1
    // KOLEJNOŚĆ JAK W M5Unified: najpierw rejestry kodeka PRZY MARTWYCH
    // zegarach, dopiero potem enable I2S — podejrzenie: sekcja ADC rusza
    // tylko przy czystym starcie zegarów PO konfiguracji
    static constexpr uint8_t seq[][2] = {
        {0x00, 0x80}, {0x01, 0xBA}, {0x02, 0x18}, {0x0D, 0x01},
        {0x0E, 0x02}, {0x14, 0x17}, {0x17, 0xBF}, {0x1C, 0x6A},
    };
    for (auto& rv : seq)
      if (!es8311Write(rv[0], rv[1])) return false;
    vTaskDelay(pdMS_TO_TICKS(20));
#else
    Serial.println("grajek: PODSLUCH — kodek nietkniety (stan po fabryce)");
#endif
    if (i2s_channel_enable(s_rx) != ESP_OK) return false;
    Serial.println("grajek: EKSPERYMENT RX-ONLY aktywny (glosnik milczy)");
    // pętla sond zamiast audioTask: czytamy i mierzymy
    xTaskCreatePinnedToCore(
        [](void*) {
          static int16_t buf[256];
          for (;;) {
            size_t got = 0;
            i2s_channel_read(s_rx, buf, sizeof(buf), &got, 1000);
            const int n = (int)(got / sizeof(int16_t));
            if (n > 0) {
              s_rxBytes.fetch_add((uint32_t)got, std::memory_order_relaxed);
              int mL = s_rxMaxL.load(std::memory_order_relaxed);
              for (int i = 0; i < n; ++i) {
                const int a = abs((int)buf[i]);
                if (a > mL) mL = a;
              }
              s_rxMaxL.store(mL, std::memory_order_relaxed);
              s_rxMaxR.store(mL, std::memory_order_relaxed);
            }
          }
        },
        "earexp", 4096, nullptr, 5, nullptr, 0);
    return true;
  }
#endif

  // TX: I2S1 master (zweryfikowany głośnik), RX: I2S0 slave na wspólnych
  // padach — na razie GŁUCHY. WNIOSEK z nocnego śledztwa: ADC ES8311 na tej
  // płytce ożywa tylko pod M5Unified Mic_Class (mic_task robi surowe,
  // pozasterownikowe dzielniki zegara i reset RX — i2s_ll_*), więc ucho v2
  // = przełączanie na M5.Mic w oknach słuchania (jak fabryczna nagrywarka),
  // nie replika przez czyste API i2s_std. Szczegóły: pamięć projektu.
  i2s_std_config_t stdCfg = {};
  stdCfg.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG((uint32_t)engine->sampleRate());
  stdCfg.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                        I2S_SLOT_MODE_STEREO);
  stdCfg.gpio_cfg.mclk = I2S_GPIO_UNUSED;  // no MCLK line — codec runs MCLK=BCLK
  stdCfg.gpio_cfg.bclk = (gpio_num_t)kPinI2sBclk;
  stdCfg.gpio_cfg.ws = (gpio_num_t)kPinI2sWs;
  stdCfg.gpio_cfg.dout = I2S_GPIO_UNUSED;
  stdCfg.gpio_cfg.din = I2S_GPIO_UNUSED;
  stdCfg.gpio_cfg.invert_flags.mclk_inv = false;
  stdCfg.gpio_cfg.invert_flags.bclk_inv = false;
  stdCfg.gpio_cfg.invert_flags.ws_inv = false;

  i2s_chan_config_t chanCfg =
      I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
  chanCfg.dma_desc_num = kAudioDmaDescs;
  chanCfg.dma_frame_num = kAudioFrames;
  if (i2s_new_channel(&chanCfg, &s_tx, nullptr) != ESP_OK) return false;
  {
    i2s_std_config_t txCfg = stdCfg;
    txCfg.gpio_cfg.dout = (gpio_num_t)kPinI2sDout;
    if (i2s_channel_init_std_mode(s_tx, &txCfg) != ESP_OK) return false;
  }

  {
    i2s_chan_config_t rxChanCfg =
        I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_SLAVE);
    rxChanCfg.dma_desc_num = kAudioDmaDescs;
    rxChanCfg.dma_frame_num = kAudioFrames;
    if (i2s_new_channel(&rxChanCfg, nullptr, &s_rx) == ESP_OK) {
      i2s_std_config_t rxCfg = stdCfg;
      rxCfg.gpio_cfg.din = (gpio_num_t)kPinI2sDin;
      if (i2s_channel_init_std_mode(s_rx, &rxCfg) != ESP_OK) {
        Serial.println("grajek: ear RX init failed — playing deaf");
        s_rx = nullptr;  // uchwyt zostaje w sterowniku; gramy dalej
      }
    }
  }

  if (s_rx) {
    // Init slave'a mógł przestawić pady zegarów — przywróć wyjścia TX
    // (I2S1) i równolegle dopnij wejścia RX (I2S0) przez matrycę GPIO.
    gpio_set_direction((gpio_num_t)kPinI2sBclk, GPIO_MODE_INPUT_OUTPUT);
    gpio_set_direction((gpio_num_t)kPinI2sWs, GPIO_MODE_INPUT_OUTPUT);
    esp_rom_gpio_connect_out_signal(kPinI2sBclk, I2S1O_BCK_OUT_IDX, false,
                                    false);
    esp_rom_gpio_connect_out_signal(kPinI2sWs, I2S1O_WS_OUT_IDX, false, false);
    esp_rom_gpio_connect_in_signal(kPinI2sBclk, I2S0I_BCK_IN_IDX, false);
    esp_rom_gpio_connect_in_signal(kPinI2sWs, I2S0I_WS_IN_IDX, false);
    PIN_INPUT_ENABLE(GPIO_PIN_MUX_REG[kPinI2sDin]);
    esp_rom_gpio_connect_in_signal(kPinI2sDin, I2S0I_SD_IN_IDX, false);
  }

  // Kodek konfigurujemy przy MARTWYCH zegarach (tak robi M5Unified) —
  // ES8311 łapie konfigurację ADC tylko przy czystym starcie zegarów.
  if (s_rx ? !es8311InitDuplex() : !es8311InitPlayback()) return false;
  vTaskDelay(pdMS_TO_TICKS(20));

  if (i2s_channel_enable(s_tx) != ESP_OK) return false;
  if (s_rx && i2s_channel_enable(s_rx) != ESP_OK) {
    Serial.println("grajek: ear RX enable failed — playing deaf");
    s_rx = nullptr;
  }

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
  Serial.printf("grajek: ucho %s\n",
                s_rx ? "OK (full-duplex TX+RX)" : "GLUCHE (brak RX)");

  const BaseType_t ok = xTaskCreatePinnedToCore(
      audioTask, "audio", 8192, nullptr, configMAX_PRIORITIES - 3, nullptr, 0);
  return ok == pdPASS;
}

}  // namespace hal
