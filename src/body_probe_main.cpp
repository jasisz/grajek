// "Pozyczone cialo" feasibility probe for the M5Stack Cardputer-ADV.
//
// This is intentionally a separate PlatformIO environment. It does not use
// Grajek's production audio engine and it never enables the microphone. A
// known speaker chirp is the stimulus; the BMI270 FIFO is the only sensor.
#include <M5Cardputer.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace {

constexpr uint8_t kBmiAddress = 0x69;
constexpr uint32_t kI2cFrequency = 400000;

constexpr uint8_t kRegChipId = 0x00;
constexpr uint8_t kRegError = 0x02;
constexpr uint8_t kRegStatus = 0x03;
constexpr uint8_t kRegIntStatus1 = 0x1D;
constexpr uint8_t kRegInternalStatus = 0x21;
constexpr uint8_t kRegFifoLength = 0x24;
constexpr uint8_t kRegFifoData = 0x26;
constexpr uint8_t kRegAccConf = 0x40;
constexpr uint8_t kRegAccRange = 0x41;
constexpr uint8_t kRegFifoDowns = 0x45;
constexpr uint8_t kRegFifoWatermark0 = 0x46;
constexpr uint8_t kRegFifoWatermark1 = 0x47;
constexpr uint8_t kRegFifoConfig0 = 0x48;
constexpr uint8_t kRegFifoConfig1 = 0x49;
constexpr uint8_t kRegPwrConf = 0x7C;
constexpr uint8_t kRegPwrCtrl = 0x7D;
constexpr uint8_t kRegCmd = 0x7E;

constexpr uint8_t kBmiChipId = 0x24;
constexpr uint8_t kFifoFlush = 0xB0;
constexpr uint16_t kImuRateHz = 1600;
constexpr uint8_t kAccelRangeG = 2;

constexpr uint32_t kAudioRateHz = 48000;
constexpr uint32_t kLeadMs = 50;
constexpr uint32_t kSweepMs = 250;
constexpr uint32_t kTailMs = 200;
constexpr uint32_t kCaptureMs = 525;
constexpr float kStartHz = 180.0f;
constexpr float kEndHz = 650.0f;
constexpr float kPcmPeak = 0.80f;
constexpr uint32_t kFadeMs = 10;

constexpr size_t samplesForMs(uint32_t ms) {
  return static_cast<size_t>(kAudioRateHz) * ms / 1000;
}

constexpr size_t kLeadSamples = samplesForMs(kLeadMs);
constexpr size_t kSweepSamples = samplesForMs(kSweepMs);
constexpr size_t kTailSamples = samplesForMs(kTailMs);
constexpr size_t kAudioSamples = kLeadSamples + kSweepSamples + kTailSamples;
constexpr size_t kFadeSamples = samplesForMs(kFadeMs);
constexpr size_t kMaxImuSamples = 1200;
constexpr size_t kFifoReadBytes = 96;  // 16 complete accel frames.

static_assert(kFifoReadBytes % 6 == 0, "FIFO reads must contain whole frames");

struct AccelSample {
  int16_t x;
  int16_t y;
  int16_t z;
};

struct SavedImuConfig {
  uint8_t accConf = 0;
  uint8_t accRange = 0;
  uint8_t fifoDowns = 0;
  uint8_t fifoWatermark0 = 0;
  uint8_t fifoWatermark1 = 0;
  uint8_t fifoConfig0 = 0;
  uint8_t fifoConfig1 = 0;
  uint8_t pwrConf = 0;
  uint8_t pwrCtrl = 0;
  bool valid = false;
};

struct CaptureResult {
  size_t count = 0;
  uint16_t fifoPeakBytes = 0;
  uint32_t i2cErrors = 0;
  bool fifoNearFull = false;
  bool fifoError = false;
  bool fifoMisaligned = false;
  bool sampleBufferFull = false;
  bool saturated = false;
  bool transportFailed = false;
  bool playOk = true;
};

int16_t audioBuffer[kAudioSamples];
AccelSample imuSamples[kMaxImuSamples];
SavedImuConfig savedImu;
uint32_t runNumber = 0;
uint8_t speakerVolume = 128;
bool displayOk = false;
bool speakerOk = false;
bool imuOk = false;

bool readRegisters(uint8_t reg, uint8_t* data, size_t length,
                   uint32_t* errorCount = nullptr) {
  const bool ok = M5.In_I2C.readRegister(kBmiAddress, reg, data, length,
                                         kI2cFrequency);
  if (!ok && errorCount != nullptr) ++*errorCount;
  return ok;
}

bool writeRegister(uint8_t reg, uint8_t value,
                   uint32_t* errorCount = nullptr) {
  const bool ok = M5.In_I2C.writeRegister8(kBmiAddress, reg, value,
                                           kI2cFrequency);
  if (!ok && errorCount != nullptr) ++*errorCount;
  return ok;
}

bool readRegister(uint8_t reg, uint8_t& value) {
  return readRegisters(reg, &value, 1);
}

void generateChirp() {
  std::memset(audioBuffer, 0, sizeof(audioBuffer));

  constexpr double kPi = 3.14159265358979323846;
  const double duration = static_cast<double>(kSweepSamples - 1) /
                          static_cast<double>(kAudioRateHz);
  const double slope = (static_cast<double>(kEndHz) - kStartHz) / duration;

  for (size_t n = 0; n < kSweepSamples; ++n) {
    const double t = static_cast<double>(n) / kAudioRateHz;
    const double phase =
        2.0 * kPi * (static_cast<double>(kStartHz) * t +
                     0.5 * slope * t * t);

    double envelope = 1.0;
    if (n < kFadeSamples) {
      envelope = 0.5 - 0.5 * std::cos(kPi * static_cast<double>(n) /
                                      static_cast<double>(kFadeSamples - 1));
    } else if (n >= kSweepSamples - kFadeSamples) {
      const size_t remaining = kSweepSamples - 1 - n;
      envelope = 0.5 - 0.5 * std::cos(
                                 kPi * static_cast<double>(remaining) /
                                 static_cast<double>(kFadeSamples - 1));
    }

    const double value = kPcmPeak * 32767.0 * envelope * std::sin(phase);
    audioBuffer[kLeadSamples + n] = static_cast<int16_t>(std::lrint(value));
  }
}

bool saveImuConfig() {
  savedImu.valid =
      readRegister(kRegAccConf, savedImu.accConf) &&
      readRegister(kRegAccRange, savedImu.accRange) &&
      readRegister(kRegFifoDowns, savedImu.fifoDowns) &&
      readRegister(kRegFifoWatermark0, savedImu.fifoWatermark0) &&
      readRegister(kRegFifoWatermark1, savedImu.fifoWatermark1) &&
      readRegister(kRegFifoConfig0, savedImu.fifoConfig0) &&
      readRegister(kRegFifoConfig1, savedImu.fifoConfig1) &&
      readRegister(kRegPwrConf, savedImu.pwrConf) &&
      readRegister(kRegPwrCtrl, savedImu.pwrCtrl);
  return savedImu.valid;
}

bool waitForCommandReady(CaptureResult& result) {
  for (int retry = 0; retry < 10; ++retry) {
    uint8_t status = 0;
    if (!readRegisters(kRegStatus, &status, 1, &result.i2cErrors)) return false;
    if ((status & 0x10) != 0) return true;
    delay(1);
  }
  return false;
}

bool flushFifo(CaptureResult& result) {
  if (!waitForCommandReady(result) ||
      !writeRegister(kRegCmd, kFifoFlush, &result.i2cErrors)) {
    return false;
  }
  return waitForCommandReady(result);
}

bool configureImuForProbe(CaptureResult& result) {
  // Disable advanced power save, stop sensors while changing ODR/range, then
  // enable only the accelerometer. ACC_CONF=0xAC means performance mode,
  // normal filtering and 1600 Hz ODR. Headerless FIFO frames are exactly XYZ.
  bool ok = true;
  ok &= writeRegister(kRegPwrConf, 0x00, &result.i2cErrors);
  delay(1);
  ok &= writeRegister(kRegFifoConfig1, 0x00, &result.i2cErrors);
  ok &= writeRegister(kRegPwrCtrl, 0x00, &result.i2cErrors);
  delay(2);
  ok &= writeRegister(kRegAccConf, 0xAC, &result.i2cErrors);
  ok &= writeRegister(kRegAccRange, 0x00, &result.i2cErrors);  // +/- 2 g
  ok &= writeRegister(kRegFifoDowns, 0x80, &result.i2cErrors);
  ok &= writeRegister(kRegFifoConfig0, 0x01, &result.i2cErrors);
  // FIFO remains disabled during readback/verification. It is enabled by the
  // final I2C write immediately before the capture timeline starts.
  ok &= writeRegister(kRegPwrCtrl, 0x04, &result.i2cErrors);
  delay(4);
  ok &= flushFifo(result);

  const struct {
    uint8_t reg;
    uint8_t expected;
  } checks[] = {{kRegAccConf, 0xAC},       {kRegAccRange, 0x00},
                {kRegFifoDowns, 0x80},     {kRegFifoConfig0, 0x01},
                {kRegFifoConfig1, 0x00},   {kRegPwrConf, 0x00},
                {kRegPwrCtrl, 0x04}};
  for (const auto& check : checks) {
    uint8_t actual = 0;
    const bool readOk =
        readRegisters(check.reg, &actual, 1, &result.i2cErrors);
    ok &= readOk && actual == check.expected;
  }
  return ok;
}

void restoreImu(CaptureResult& result) {
  writeRegister(kRegFifoConfig1, 0x00, &result.i2cErrors);
  writeRegister(kRegPwrCtrl, 0x00, &result.i2cErrors);
  writeRegister(kRegCmd, kFifoFlush, &result.i2cErrors);
  delay(2);

  if (!savedImu.valid) return;

  writeRegister(kRegAccConf, savedImu.accConf, &result.i2cErrors);
  writeRegister(kRegAccRange, savedImu.accRange, &result.i2cErrors);
  writeRegister(kRegFifoDowns, savedImu.fifoDowns, &result.i2cErrors);
  writeRegister(kRegFifoWatermark0, savedImu.fifoWatermark0,
                &result.i2cErrors);
  writeRegister(kRegFifoWatermark1, savedImu.fifoWatermark1,
                &result.i2cErrors);
  writeRegister(kRegFifoConfig0, savedImu.fifoConfig0, &result.i2cErrors);
  writeRegister(kRegPwrCtrl, savedImu.pwrCtrl, &result.i2cErrors);
  if ((savedImu.pwrCtrl & 0x02) != 0) delay(45);  // gyro startup
  writeRegister(kRegCmd, kFifoFlush, &result.i2cErrors);
  delay(2);
  writeRegister(kRegFifoConfig1, savedImu.fifoConfig1, &result.i2cErrors);
  writeRegister(kRegPwrConf, savedImu.pwrConf, &result.i2cErrors);
  if ((savedImu.pwrConf & 0x01) != 0) delay(1);
}

uint16_t fifoLength(CaptureResult& result) {
  uint8_t lengthBytes[2] = {};
  if (!readRegisters(kRegFifoLength, lengthBytes, sizeof(lengthBytes),
                     &result.i2cErrors)) {
    result.transportFailed = true;
    return 0;
  }
  return static_cast<uint16_t>(lengthBytes[0]) |
         (static_cast<uint16_t>(lengthBytes[1] & 0x3F) << 8);
}

void drainFifo(CaptureResult& result) {
  uint8_t bytes[kFifoReadBytes];

  for (;;) {
    const uint16_t available = fifoLength(result);
    if (result.transportFailed) return;
    if (available > result.fifoPeakBytes) result.fifoPeakBytes = available;
    if (available >= 1800) result.fifoNearFull = true;
    if (available % 6 != 0) result.fifoMisaligned = true;

    const size_t completeBytes = available - available % 6;
    if (completeBytes == 0) return;

    const size_t toRead = completeBytes < sizeof(bytes) ? completeBytes
                                                        : sizeof(bytes);
    if (!readRegisters(kRegFifoData, bytes, toRead, &result.i2cErrors)) {
      result.transportFailed = true;
      return;
    }

    for (size_t offset = 0; offset < toRead; offset += 6) {
      const auto decode = [&bytes](size_t index) {
        return static_cast<int16_t>(
            static_cast<uint16_t>(bytes[index]) |
            (static_cast<uint16_t>(bytes[index + 1]) << 8));
      };

      if (result.count < kMaxImuSamples) {
        const AccelSample sample = {decode(offset), decode(offset + 2),
                                    decode(offset + 4)};
        imuSamples[result.count] = sample;
        if (sample.x <= -32760 || sample.x >= 32760 || sample.y <= -32760 ||
            sample.y >= 32760 || sample.z <= -32760 || sample.z >= 32760) {
          result.saturated = true;
        }
        ++result.count;
      } else {
        result.sampleBufferFull = true;
      }
    }
  }
}

void readFifoDiagnostics(CaptureResult& result) {
  uint8_t error = 0;
  uint8_t intStatus = 0;
  if (!readRegisters(kRegError, &error, 1, &result.i2cErrors) ||
      !readRegisters(kRegIntStatus1, &intStatus, 1, &result.i2cErrors)) {
    result.transportFailed = true;
    return;
  }
  if ((error & 0x40) != 0 || (intStatus & 0x01) != 0) {
    result.fifoError = true;
  }
}

void drawStatus(const char* state, uint16_t color) {
  if (!displayOk) return;

  auto& display = M5Cardputer.Display;
  display.fillScreen(TFT_BLACK);
  display.setTextDatum(top_left);
  display.setTextColor(TFT_CYAN, TFT_BLACK);
  display.setTextSize(2);
  display.setCursor(8, 8);
  display.println("POZYCZONE CIALO");
  display.setTextSize(1);
  display.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  display.setCursor(8, 38);
  display.println("Sonda: glosnik -> BMI270 FIFO");
  display.println("Mikrofon: WYLACZONY");
  display.setTextColor(color, TFT_BLACK);
  display.setTextSize(2);
  display.setCursor(8, 70);
  display.println(state);
  display.setTextSize(1);
  display.setTextColor(TFT_WHITE, TFT_BLACK);
  display.setCursor(8, 103);
  display.printf("GO / USB g   VOL %u\n", speakerVolume);
  display.setCursor(8, 119);
  display.print("USB n = sama cisza");
}

void printReady() {
  Serial.printf(
      "BODY_PROBE_READY version=1 volume=%u imu_ok=%u speaker_ok=%u\n",
      speakerVolume, imuOk ? 1 : 0, speakerOk ? 1 : 0);
}

void dumpCapture(const CaptureResult& result, bool withChirp) {
  const char* stimulus = withChirp ? "chirp" : "noise";
  Serial.printf(
      "# BODY_PROBE_BEGIN version=1 run=%lu stimulus=%s imu_rate_hz=%u "
      "range_g=%u audio_rate_hz=%lu lead_ms=%lu sweep_ms=%lu tail_ms=%lu "
      "capture_ms=%lu f0_hz=%.1f f1_hz=%.1f volume=%u\n",
      static_cast<unsigned long>(runNumber), stimulus, kImuRateHz,
      kAccelRangeG, static_cast<unsigned long>(kAudioRateHz),
      static_cast<unsigned long>(kLeadMs),
      static_cast<unsigned long>(kSweepMs),
      static_cast<unsigned long>(kTailMs),
      static_cast<unsigned long>(kCaptureMs), kStartHz, kEndHz,
      speakerVolume);
  Serial.println("sample,x,y,z");
  for (size_t i = 0; i < result.count; ++i) {
    Serial.printf("%u,%d,%d,%d\n", static_cast<unsigned>(i), imuSamples[i].x,
                  imuSamples[i].y, imuSamples[i].z);
  }
  const size_t expected = static_cast<size_t>(kImuRateHz) * kCaptureMs / 1000;
  const size_t countTolerance = expected / 50;  // BMI ODR + final drain: 2%
  const bool countOk = result.count + countTolerance >= expected &&
                       result.count <= expected + countTolerance;
  const bool valid = countOk && result.i2cErrors == 0 && !result.fifoNearFull &&
                     !result.fifoError && !result.fifoMisaligned &&
                     !result.sampleBufferFull && !result.saturated &&
                     !result.transportFailed && result.playOk;
  Serial.printf(
      "# BODY_PROBE_END run=%lu samples=%u expected=%u fifo_peak_bytes=%u "
      "fifo_near_full=%u fifo_error=%u fifo_misaligned=%u buffer_full=%u "
      "saturated=%u transport_failed=%u i2c_errors=%lu play_ok=%u valid=%u\n",
      static_cast<unsigned long>(runNumber),
      static_cast<unsigned>(result.count), static_cast<unsigned>(expected),
      result.fifoPeakBytes, result.fifoNearFull ? 1 : 0,
      result.fifoError ? 1 : 0, result.fifoMisaligned ? 1 : 0,
      result.sampleBufferFull ? 1 : 0, result.saturated ? 1 : 0,
      result.transportFailed ? 1 : 0,
      static_cast<unsigned long>(result.i2cErrors), result.playOk ? 1 : 0,
      valid ? 1 : 0);
}

void runProbe(bool withChirp) {
  if (!imuOk || (withChirp && !speakerOk)) {
    Serial.println("BODY_PROBE_ERROR unavailable_hardware");
    drawStatus("BLAD SPRZETU", TFT_RED);
    return;
  }

  ++runNumber;
  CaptureResult result;
  drawStatus(withChirp ? "MIERZE CHIRP..." : "MIERZE CISZE...", TFT_YELLOW);
  Serial.printf("BODY_PROBE_MEASURING run=%lu stimulus=%s\n",
                static_cast<unsigned long>(runNumber),
                withChirp ? "chirp" : "noise");

  if (!configureImuForProbe(result)) {
    restoreImu(result);
    Serial.println("BODY_PROBE_ERROR imu_configuration");
    drawStatus("BLAD IMU", TFT_RED);
    return;
  }

  // The accelerometer is running but FIFO is still disabled. A final flush
  // here, then one write enabling FIFO below, gives the samples and leading
  // audio silence a common timeline origin to within one I2C transaction.
  if (!flushFifo(result)) {
    restoreImu(result);
    Serial.println("BODY_PROBE_ERROR fifo_flush");
    drawStatus("BLAD FIFO", TFT_RED);
    return;
  }

  if (!writeRegister(kRegFifoConfig1, 0x40, &result.i2cErrors)) {
    restoreImu(result);
    Serial.println("BODY_PROBE_ERROR fifo_enable");
    drawStatus("BLAD FIFO", TFT_RED);
    return;
  }

  const uint32_t startedUs = micros();
  if (withChirp) {
    result.playOk = M5.Speaker.playRaw(audioBuffer, kAudioSamples,
                                       kAudioRateHz, false, 1, 0, true);
  }

  while (static_cast<uint32_t>(micros() - startedUs) < kCaptureMs * 1000UL) {
    drainFifo(result);
    if (result.transportFailed) break;
    delayMicroseconds(1500);
  }
  drainFifo(result);
  readFifoDiagnostics(result);

  if (withChirp) M5.Speaker.stop(0);
  restoreImu(result);

  drawStatus("WYSYLAM CSV...", TFT_GREEN);
  dumpCapture(result, withChirp);
  drawStatus("GOTOWY", TFT_GREEN);
  printReady();
}

void adjustVolume(int delta) {
  int next = static_cast<int>(speakerVolume) + delta;
  if (next < 32) next = 32;
  if (next > 224) next = 224;
  speakerVolume = static_cast<uint8_t>(next);
  M5.Speaker.setVolume(speakerVolume);
  drawStatus("GOTOWY", TFT_GREEN);
  printReady();
}

}  // namespace

void setup() {
  Serial.begin(115200);

  auto cfg = M5.config();
  cfg.internal_spk = true;
  cfg.internal_mic = false;
  cfg.internal_imu = true;
  cfg.internal_rtc = false;
  M5Cardputer.begin(cfg, false);  // no TCA8418 traffic during measurements

  displayOk = M5Cardputer.Display.getPanel() != nullptr &&
              M5Cardputer.Display.width() > 0;
  if (displayOk) {
    M5Cardputer.Display.setRotation(1);
    M5Cardputer.Display.setBrightness(160);
  }

  uint8_t chipId = 0;
  uint8_t internalStatus = 0;
  imuOk = readRegister(kRegChipId, chipId) && chipId == kBmiChipId &&
          readRegister(kRegInternalStatus, internalStatus) &&
          (internalStatus & 0x0F) == 0x01 && saveImuConfig();

  auto speakerConfig = M5.Speaker.config();
  speakerConfig.sample_rate = kAudioRateHz;
  speakerConfig.stereo = false;
  speakerConfig.i2s_port = I2S_NUM_1;
  speakerConfig.pin_bck = 41;
  speakerConfig.pin_ws = 43;
  speakerConfig.pin_data_out = 42;
  speakerConfig.pin_mck = I2S_PIN_NO_CHANGE;
  speakerConfig.magnification = 16;
  speakerConfig.dma_buf_len = 128;
  speakerConfig.dma_buf_count = 4;
  speakerConfig.task_priority = 2;
  speakerConfig.task_pinned_core = 0;
  M5.Speaker.end();
  M5.Speaker.config(speakerConfig);
  M5.Speaker.setVolume(speakerVolume);
  M5.Speaker.setChannelVolume(0, 255);
  speakerOk = M5.Speaker.begin();
  delay(20);  // let the codec and DMA settle before a possible first run

  generateChirp();
  drawStatus((imuOk && speakerOk) ? "GOTOWY" : "BLAD SPRZETU",
             (imuOk && speakerOk) ? TFT_GREEN : TFT_RED);
  Serial.printf(
      "BODY_PROBE_BOOT chip_id=0x%02X internal_status=0x%02X display_ok=%u\n",
      chipId, internalStatus, displayOk ? 1 : 0);
  printReady();
}

void loop() {
  M5Cardputer.update();
  if (M5Cardputer.BtnA.wasPressed()) runProbe(true);

  while (Serial.available() > 0) {
    switch (Serial.read()) {
      case 'g':
      case 'G':
        runProbe(true);
        break;
      case 'n':
      case 'N':
        runProbe(false);
        break;
      case '+':
        adjustVolume(16);
        break;
      case '-':
        adjustVolume(-16);
        break;
      case '?':
        printReady();
        break;
      default:
        break;
    }
  }

  delay(5);
}
