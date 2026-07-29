// PC "live" target: play the engine IN REAL TIME through CoreAudio (macOS,
// zero dependencies). The laptop keyboard mimics the Cardputer grid: 4 QWERTY
// rows = 4 grid rows (bottom row = lowest interval), column = scale step.
//
//   1 2 3 4 5 6 7 8 9 0 - =     <- row 3 (highest)
//   q w e r t y u i o p [ ]     <- row 2
//   a s d f g h j k l ; ' \     <- row 1
//   z x c v b n m , . /         <- row 0 (lowest)
//
// A terminal never reports key releases, so:
//  - normal mode: a note decays ~0.6 s after the last repeat (holding a key
//    sustains it via the OS auto-repeat),
//  - LATCH mode (space): a press turns the note on permanently, a second
//    press turns it off — for building drones and chords.
//
// Controls: TAB scale | ` timbre | BACKSPACE base octave | SPACE latch
//           left/right arrows: pitch bend (IMU stand-in) | up/down: filter
//           ENTER panic (silence + bend reset; keeps the loop) | ESC quit
//
// Looper (records what you play, layers on top — same core as the device's
// future LOOP mode, which will be fed by the microphone instead):
//           SHIFT+R record: 1st press starts the loop, 2nd closes it,
//                   after that it toggles overdub (each take = one layer)
//           SHIFT+P play/stop | SHIFT+U undo last layer | SHIFT+C clear
#include <AudioToolbox/AudioToolbox.h>
#include <signal.h>
#include <termios.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <chrono>
#include <vector>

#include "ga_engine.h"
#include "ga_looper.h"
#include "ga_scales.h"

using namespace ga;

namespace {

constexpr int kSr = 48000;
constexpr int kFramesPerBuf = 256;  // ~5.3 ms per buffer
constexpr int kNumBufs = 3;         // ~16 ms total output latency

Engine g_engine;
Looper g_looper;
constexpr int kMaxLoopSec = 60;

// Ctrl+C must go through the cleanup path (restore termios, stop the queue),
// not kill the process mid-raw-mode.
volatile sig_atomic_t g_running = 1;
void onSignal(int) { g_running = 0; }

void aqCallback(void*, AudioQueueRef q, AudioQueueBufferRef buf) {
  const int frames = (int)(buf->mAudioDataBytesCapacity / sizeof(float));
  float* samples = (float*)buf->mAudioData;
  g_engine.process(samples, frames);
  // The looper records the dry synth and adds loop playback on top.
  g_looper.process(samples, samples, frames);
  buf->mAudioDataByteSize = (UInt32)(frames * sizeof(float));
  AudioQueueEnqueueBuffer(q, buf, 0, nullptr);
}

bool audioStart(AudioQueueRef* outQueue) {
  AudioStreamBasicDescription fmt = {};
  fmt.mSampleRate = kSr;
  fmt.mFormatID = kAudioFormatLinearPCM;
  fmt.mFormatFlags = kLinearPCMFormatFlagIsFloat | kLinearPCMFormatFlagIsPacked;
  fmt.mChannelsPerFrame = 1;
  fmt.mBitsPerChannel = 32;
  fmt.mBytesPerFrame = 4;
  fmt.mFramesPerPacket = 1;
  fmt.mBytesPerPacket = 4;
  AudioQueueRef queue = nullptr;
  if (AudioQueueNewOutput(&fmt, aqCallback, nullptr, nullptr, nullptr, 0,
                          &queue) != noErr)
    return false;
  for (int i = 0; i < kNumBufs; ++i) {
    AudioQueueBufferRef buf = nullptr;
    if (AudioQueueAllocateBuffer(queue, kFramesPerBuf * sizeof(float), &buf) !=
        noErr)
      return false;
    aqCallback(nullptr, queue, buf);  // pre-fill and enqueue
  }
  if (AudioQueueStart(queue, nullptr) != noErr) return false;
  *outQueue = queue;
  return true;
}

// --- laptop keyboard -> grid mapping ---
struct KeyPos { int col; int row; };

bool keyToGrid(char c, KeyPos& out) {
  static const char* rows[4] = {
      "zxcvbnm,./",     // row 0 — lowest
      "asdfghjkl;'\\",  // row 1
      "qwertyuiop[]",   // row 2
      "1234567890-=",   // row 3 — highest
  };
  if (c == '\0') return false;
  for (int r = 0; r < 4; ++r) {
    const char* p = strchr(rows[r], c);
    if (p) {
      out.row = r;
      out.col = (int)(p - rows[r]);
      return true;
    }
  }
  return false;
}

// --- performance state ---
struct NoteState {
  bool on = false;
  bool latched = false;
  double offAt = 0.0;
};
NoteState g_notes[64];

double nowSec() {
  using namespace std::chrono;
  return duration<double>(steady_clock::now().time_since_epoch()).count();
}

const char* kPresetNames[kNumTimbrePresets] = {"PURE", "DRONE", "REED"};
const float kBaseOctaves[4] = {55.0f, 110.0f, 220.0f, 440.0f};

struct Ui {
  int scale = (int)ScaleId::EDO19;
  int preset = 0;
  int octave = 2;  // 220 Hz
  bool latch = false;
  float cutoff = 6000.0f;
  float bend = 0.0f;
};

const char* loopStateName(Looper::State s) {
  switch (s) {
    case Looper::State::Empty:       return "--";
    case Looper::State::RecordFirst: return "REC";
    case Looper::State::Play:        return "PLAY";
    case Looper::State::Overdub:     return "DUB";
    case Looper::State::Stopped:     return "STOP";
  }
  return "?";
}

void printStatus(const Ui& ui) {
  printf("\r\x1b[K[%s] timbre:%s base:%.0fHz latch:%s filter:%.0fHz bend:%+.0fc voices:%d",
         scaleInfo((ScaleId)ui.scale).name, kPresetNames[ui.preset],
         kBaseOctaves[ui.octave], ui.latch ? "ON " : "off",
         ui.cutoff, ui.bend, g_engine.activeVoiceCount());
  const Looper::State ls = g_looper.state();
  if (ls == Looper::State::Empty) {
    printf(" loop:--");
  } else if (ls == Looper::State::RecordFirst) {
    printf(" loop:REC %.1fs", (float)g_looper.lengthFrames() / kSr);
  } else {
    printf(" loop:%s %dlyr %.1f/%.1fs vol:%.0f%%", loopStateName(ls),
           g_looper.layerCount(), (float)g_looper.positionFrames() / kSr,
           (float)g_looper.lengthFrames() / kSr,
           g_looper.playbackLevel() * 100.0f);
  }
  printf(" ");
  fflush(stdout);
}

void allOff() {
  g_engine.allNotesOff();
  for (auto& n : g_notes) n = NoteState{};
}

void sleepMs(int ms) {
  struct timespec ts = {ms / 1000, (long)(ms % 1000) * 1000000L};
  nanosleep(&ts, nullptr);
}

int selftest() {
  AudioQueueRef queue = nullptr;
  if (!audioStart(&queue)) {
    fprintf(stderr, "selftest: failed to open CoreAudio output\n");
    return 1;
  }
  g_engine.noteOn(1, 0.0f, 0.9f);
  sleepMs(700);
  g_engine.noteOff(1);
  sleepMs(500);
  AudioQueueStop(queue, true);
  AudioQueueDispose(queue, true);
  printf("selftest ok — did you hear the tone? (A = 220 Hz)\n");
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  g_engine.init(kSr);
  g_engine.setParam(Param::TimbrePreset, 0);
  g_engine.setParam(Param::BaseHz, 220.0f);
  g_engine.setParam(Param::FilterCutoffHz, 6000.0f);

  // Loop storage lives for the whole run; allocated before audio starts.
  static std::vector<int16_t> mixBuf((size_t)kMaxLoopSec * kSr);
  static std::vector<int16_t> layerBuf((size_t)kMaxLoopSec * kSr);
  static std::vector<int16_t> undoBuf((size_t)kMaxLoopSec * kSr);
  g_looper.init(mixBuf.data(), layerBuf.data(), undoBuf.data(),
                (uint32_t)mixBuf.size());
  g_looper.setPlaybackLevel(0.8f);  // leave headroom to play over the loop

  if (argc > 1 && strcmp(argv[1], "--selftest") == 0) return selftest();

  if (!isatty(STDIN_FILENO)) {
    fprintf(stderr, "grajek_live needs a terminal (tty). Run without a pipe.\n");
    return 1;
  }

  AudioQueueRef queue = nullptr;
  if (!audioStart(&queue)) {
    fprintf(stderr, "Failed to open the CoreAudio output.\n");
    return 1;
  }

  struct sigaction sa = {};
  sa.sa_handler = onSignal;
  sigaction(SIGINT, &sa, nullptr);
  sigaction(SIGTERM, &sa, nullptr);

  termios orig{};
  tcgetattr(STDIN_FILENO, &orig);
  termios raw = orig;
  raw.c_lflag &= ~(ICANON | ECHO);
  raw.c_cc[VMIN] = 0;
  raw.c_cc[VTIME] = 1;  // read blocks for at most 100 ms
  tcsetattr(STDIN_FILENO, TCSANOW, &raw);

  printf("grajek_live — engine at %d Hz, ~16 ms latency\n", kSr);
  printf("  grid: keyboard rows = instrument rows (bottom = lowest)\n");
  printf("  TAB scale | ` timbre | BACKSPACE octave | SPACE latch (drones!)\n");
  printf("  arrows <- -> bend | up/down filter | ENTER panic | ESC quit\n");
  printf("  LOOPER: SHIFT+R rec/close/overdub | SHIFT+P play/stop"
         " | SHIFT+U undo | SHIFT+C clear (max %ds)\n"
         "          SHIFT+[ / SHIFT+] loop volume down/up\n\n", kMaxLoopSec);

  Ui ui;
  printStatus(ui);
  double lastStatusAt = nowSec();

  while (g_running) {
    char c = 0;
    const ssize_t n = read(STDIN_FILENO, &c, 1);

    if (n == 1) {
      KeyPos kp;
      if (c == 0x1B) {  // ESC alone, or an escape sequence
        char c2 = 0, c3 = 0;
        if (read(STDIN_FILENO, &c2, 1) != 1) {
          g_running = 0;  // read timed out: bare ESC = quit
        } else if (c2 == '[' && read(STDIN_FILENO, &c3, 1) == 1) {
          switch (c3) {
            case 'A': ui.cutoff = clampf(ui.cutoff * 1.15f, 200.0f, 12000.0f); break;
            case 'B': ui.cutoff = clampf(ui.cutoff / 1.15f, 200.0f, 12000.0f); break;
            case 'C': ui.bend = clampf(ui.bend + 25.0f, -300.0f, 300.0f); break;
            case 'D': ui.bend = clampf(ui.bend - 25.0f, -300.0f, 300.0f); break;
            default: break;
          }
          g_engine.setParam(Param::FilterCutoffHz, ui.cutoff);
          g_engine.setParam(Param::BendCents, ui.bend);
        }
        // any other sequence (Alt+key, F-keys): ignore, keep playing
      } else if (c == '\t') {
        ui.scale = (ui.scale + 1) % (int)ScaleId::Count;
        allOff();  // clean retuning
      } else if (c == '`') {
        ui.preset = (ui.preset + 1) % kNumTimbrePresets;
        g_engine.setParam(Param::TimbrePreset, (float)ui.preset);
      } else if (c == 0x7F || c == 0x08) {  // backspace
        ui.octave = (ui.octave + 1) % 4;
        g_engine.setParam(Param::BaseHz, kBaseOctaves[ui.octave]);
      } else if (c == ' ') {
        ui.latch = !ui.latch;
      } else if (c == 'R') {
        g_looper.toggleRecord();
      } else if (c == 'P') {
        g_looper.togglePlay();
      } else if (c == 'U') {
        g_looper.undoLayer();
      } else if (c == 'C') {
        g_looper.clear();
      } else if (c == '{') {
        g_looper.setPlaybackLevel(g_looper.playbackLevel() - 0.1f);
      } else if (c == '}') {
        g_looper.setPlaybackLevel(g_looper.playbackLevel() + 0.1f);
      } else if (c == '\r' || c == '\n') {
        // panic silences the synth but leaves the loop running
        allOff();
        ui.bend = 0.0f;
        g_engine.setParam(Param::BendCents, 0.0f);
      } else if (keyToGrid(c, kp)) {
        const int id = kp.row * 14 + kp.col;
        const float cents = gridToCents((ScaleId)ui.scale, kp.col, kp.row);
        NoteState& st = g_notes[id];
        if (ui.latch) {
          if (st.on) {
            g_engine.noteOff(id);
            st = NoteState{};
          } else {
            g_engine.noteOn(id, cents, 0.9f);
            st.on = true;
            st.latched = true;
          }
        } else {
          // press/auto-repeat plays and extends; fades out 0.6 s later
          g_engine.noteOn(id, cents, 0.9f);
          st.on = true;
          st.latched = false;
          st.offAt = nowSec() + 0.6;
        }
      }
      printStatus(ui);
      lastStatusAt = nowSec();
    }

    // keep the loop position/state on screen while idling
    if (g_looper.state() != Looper::State::Empty &&
        nowSec() - lastStatusAt > 0.25) {
      printStatus(ui);
      lastStatusAt = nowSec();
    }

    // expire non-latched notes
    const double t = nowSec();
    for (int id = 0; id < 64; ++id) {
      if (g_notes[id].on && !g_notes[id].latched && g_notes[id].offAt <= t) {
        g_engine.noteOff(id);
        g_notes[id].on = false;
      }
    }
  }

  allOff();
  sleepMs(150);  // let the release tail ring before closing the queue
  AudioQueueStop(queue, true);
  AudioQueueDispose(queue, true);
  tcsetattr(STDIN_FILENO, TCSANOW, &orig);
  printf("\nbye!\n");
  return 0;
}
