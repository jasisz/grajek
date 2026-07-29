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
//  - LATCH mode (SHIFT+L): a press turns the note on permanently, a second
//    press turns it off — for building drones and chords.
//
// Controls: TAB scale | ` timbre | BACKSPACE base octave | SHIFT+L latch
//           left/right arrows: pitch bend (IMU stand-in) | up/down: filter
//           ENTER panic (silence + bend + re-center; keeps the loop) | ESC quit
//
// Looper (records what you play, layers on top — same core as the device's
// future LOOP mode, fed there by the microphone):
//           SHIFT+R record: 1st press starts the loop, 2nd closes it,
//                   after that it toggles overdub (each take = one layer)
//           SHIFT+P play/stop | SHIFT+U undo last layer | SHIFT+C clear
//           SHIFT+[ / SHIFT+] loop volume down/up
//
// TOSS SIMULATOR (prototype of the device's throw gesture — on hardware the
// IMU drives this; here the keyboard fakes it):
//           SPACE 1st press = throw: the WHOLE music (synth + loop varispeed)
//                 lifts in a glissando for as long as it "flies"
//           left/right arrows DURING flight = spin (each press adds a flip;
//                 right = land upward/otonal, left = land downward/utonal;
//                 more spin = audible wobble in flight + dizzy landing)
//           SPACE 2nd press = catch: flight time picks the rung of the JI
//                 ladder (9/8, 5/4, 3/2, 7/4, 2/1) and the whole music lands
//                 re-rooted on the new tonal center
//           SHIFT+X = fumble: the music falls (tape dive) and the top loop
//                 layer is lost; during flight = crash landing (no modulation)
#include <AudioToolbox/AudioToolbox.h>
#include <signal.h>
#include <termios.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <chrono>
#include <functional>
#include <vector>

#include <stdlib.h>

#include "ga_echo.h"
#include "ga_engine.h"
#include "ga_looper.h"
#include "ga_reverb.h"
#include "ga_scales.h"
#include "ga_strings.h"
#include "wav_writer.h"

using namespace ga;

namespace {

constexpr int kSr = 48000;
constexpr int kFramesPerBuf = 256;  // ~5.3 ms per buffer
constexpr int kNumBufs = 3;         // ~16 ms total output latency

Engine g_engine;
Looper g_looper;
EchoTape g_echo;   // always-on ambient memory: play anything, it comes back
Reverb g_reverb;   // the single biggest "sounds pretty by itself" ingredient
constexpr int kMaxLoopSec = 60;
constexpr float kEchoSec = 4.0f;
// the echo's raw tape, shared with the FREEZE capture
int16_t* g_echoStorage = nullptr;
uint32_t g_echoFrames = 0;

// The background (tampura): a quiet pedal under everything — random notes
// over a drone sound intentional. Default is 1/1 + 3/2; the player can PICK
// their own chord (up to 4 notes) in pick mode (SHIFT+D here; on the device
// this will be hold-a-modifier + press grid keys). Follows toss modulations
// automatically (voices track BaseHz live).
bool g_tampura = true;
bool g_pickMode = false;
struct BgNote { float cents; int32_t id; };
BgNote g_bg[4] = {{0.0f, 1000}, {702.0f, 1001}, {0.0f, -1}, {0.0f, -1}};
int g_bgCount = 2;
bool g_bgCustom = false;   // a hand-picked background silences the weather vote
int32_t g_bgNextId = 2;    // rotates through ids 1000..1015

SympatheticStrings g_strings;  // JI halo around everything (SHIFT+S toggles)

// The breathing system ("weather"): a fast envelope follower on the dry
// synth (published from the audio thread) + slow incommensurate tides,
// consumed ONLY by the main loop — single-writer rule for every parameter.
std::atomic<float> g_env{0.0f};
struct Weather {
  double start = 0.0;
  double lastTick = 0.0;
  int fifthVariant = 0;  // 0 = 3/2, 1 = 7/4 (default background only)
};
Weather g_weather;

// Ghost garden: minutes-scale memory. After ~12 s of silence the box quietly
// re-sows the player's own notes from the recent past (soft attack, low
// velocity, sometimes transposed by a pure interval). The player's first
// real key silences the ghosts immediately — it never fights the human.
struct GhostGarden {
  struct Mem { float cents; double t; };
  static constexpr int kCap = 32;
  Mem ring[kCap];
  int head = 0, count = 0;
  double lastInput = 0.0;
  double nextAt = 0.0;
  uint32_t activeMask = 0;  // which of the 4 ghost ids are sounding
  int nextSlot = 0;
};
GhostGarden g_garden;
constexpr int32_t kGhostIdBase = 1100;
constexpr double kGhostIdleSec = 12.0;

// Session recorder: the final mix (post-everything), armed with SHIFT+W.
std::atomic<bool> g_recOn{false};
std::atomic<uint32_t> g_recIdx{0};
std::vector<int16_t>* g_recBuf = nullptr;

void backgroundApply() {
  for (int i = 0; i < g_bgCount; ++i) {
    if (g_tampura)
      g_engine.noteOn(g_bg[i].id, g_bg[i].cents, i == 0 ? 0.22f : 0.16f);
    else
      g_engine.noteOff(g_bg[i].id);
  }
}

// Pick mode: a grid click toggles that pitch in the background chord.
// Full (4 notes) -> the oldest bows out. The choice is law: weather voting
// stops touching a custom background.
void backgroundToggleNote(float cents) {
  g_bgCustom = true;
  for (int i = 0; i < g_bgCount; ++i) {
    if (fabsf(g_bg[i].cents - cents) < 1.0f) {
      g_engine.noteOff(g_bg[i].id);
      for (int j = i; j < g_bgCount - 1; ++j) g_bg[j] = g_bg[j + 1];
      --g_bgCount;
      return;
    }
  }
  if (g_bgCount == 4) {
    g_engine.noteOff(g_bg[0].id);
    for (int j = 0; j < 3; ++j) g_bg[j] = g_bg[j + 1];
    --g_bgCount;
  }
  const int32_t id = 1000 + (g_bgNextId++ & 15);
  g_bg[g_bgCount] = {cents, id};
  ++g_bgCount;
  if (g_tampura)
    g_engine.noteOn(id, cents, g_bgCount == 1 ? 0.22f : 0.16f);
}

// Ctrl+C must go through the cleanup path (restore termios, stop the queue),
// not kill the process mid-raw-mode.
volatile sig_atomic_t g_running = 1;
void onSignal(int) { g_running = 0; }

void aqCallback(void*, AudioQueueRef q, AudioQueueBufferRef buf) {
  const int frames = (int)(buf->mAudioDataBytesCapacity / sizeof(float));
  float* samples = (float*)buf->mAudioData;
  g_engine.process(samples, frames);
  // Envelope follower on the dry synth (attack ~5 ms, release ~2 s) — the
  // audio thread only PUBLISHES; all modulation happens in the main loop.
  static float envLocal = 0.0f;
  for (int i = 0; i < frames; ++i) {
    const float a = fabsf(samples[i]);
    envLocal += (a > envLocal ? 0.004f : 0.00001f) * (a - envLocal);
  }
  g_env.store(envLocal, std::memory_order_relaxed);
  // Sympathetic strings: the dry synth excites the JI halo.
  g_strings.process(samples, samples, frames);
  // Generosity layer: everything played joins a fading ambient memory.
  g_echo.process(samples, samples, frames);
  // The manual looper records synth+ambience and adds loop playback on top.
  g_looper.process(samples, samples, frames);
  // Reverb last, so the loop and echo sit in the same room.
  g_reverb.process(samples, frames);
  // session recorder taps the very end of the chain
  if (g_recOn.load(std::memory_order_relaxed) && g_recBuf) {
    uint32_t w = g_recIdx.load(std::memory_order_relaxed);
    const uint32_t cap = (uint32_t)g_recBuf->size();
    for (int i = 0; i < frames && w < cap; ++i) {
      float s = samples[i];
      if (s > 1.0f) s = 1.0f;
      if (s < -1.0f) s = -1.0f;
      (*g_recBuf)[w++] = (int16_t)(s * 32767.0f);
    }
    g_recIdx.store(w, std::memory_order_relaxed);
  }
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

const char* kPresetNames[kNumTimbrePresets] = {"PURE", "DRONE", "REED",
                                               "CHIME"};
const float kBaseOctaves[4] = {55.0f, 110.0f, 220.0f, 440.0f};

// JI ladder for toss landings; sign of spin mirrors it downward (utonal).
constexpr int kNumRungs = 5;
const float kRungCents[kNumRungs] = {203.9f, 386.3f, 702.0f, 968.8f, 1200.0f};
const char* kRungNames[kNumRungs] = {"9/8", "5/4", "3/2", "7/4", "2/1"};

struct Ui {
  int scale = (int)ScaleId::PENTA;  // the happy default — no wrong keys
  int preset = 3;  // CHIME — the "pretty by itself" default
  int octave = 2;  // 220 Hz
  bool latch = false;
  float cutoff = 7500.0f;   // player's base — the weather drifts around it
  float wetBase = 0.35f;    // player's reverb base (SHIFT+V), same deal
  float bend = 0.0f;
};

// --- toss simulator state ---
struct Sim {
  bool inFlight = false;
  double t0 = 0.0;
  int spin = 0;             // arrow presses during flight; sign = direction
  float centerCents = 0.0f; // accumulated tonal-center offset
  float rateBase = 1.0f;    // loop varispeed matching the center
};
Sim g_sim;

struct TimedFn {
  double t;
  std::function<void()> fn;
};
std::vector<TimedFn> g_pending;

void schedule(double delaySec, std::function<void()> fn) {
  g_pending.push_back({nowSec() + delaySec, std::move(fn)});
}

void runPending() {
  const double t = nowSec();
  for (size_t i = 0; i < g_pending.size();) {
    if (g_pending[i].t <= t) {
      g_pending[i].fn();
      g_pending.erase(g_pending.begin() + (long)i);
    } else {
      ++i;
    }
  }
}

void applyCenter(const Ui& ui) {
  const float root =
      kBaseOctaves[ui.octave] * exp2f(g_sim.centerCents / 1200.0f);
  g_engine.setParam(Param::BaseHz, root);
  g_strings.setRootHz(root);  // the halo bends onto the new center
}

// ~30 Hz from the main loop: playing keeps the path clear; going quiet for
// a few seconds lets echo and reverb swell into the space; three
// incommensurate tides (90/210/330 s) drift the macro state so the total
// never repeats; the tampura fifth gets re-voted every few minutes.
// Ranges are deliberately narrow — bad weather must not exist.
void weatherTick(const Ui& ui) {
  const double now = nowSec();
  if (now - g_weather.lastTick < 0.033) return;
  g_weather.lastTick = now;
  const double t = now - g_weather.start;
  const float t1 = sinf((float)(kTau * t / 90.0));
  const float t2 = sinf((float)(kTau * t / 210.0));
  const float t3 = sinf((float)(kTau * t / 330.0));
  const float env = g_env.load(std::memory_order_relaxed);
  const float breathe = clampf(1.0f - (env - 0.04f) / 0.18f, 0.0f, 1.0f);

  g_echo.setLevel(clampf(0.45f + 0.25f * breathe + 0.05f * t1, 0.30f, 0.80f));
  g_echo.setFeedback(
      clampf(0.52f + 0.10f * breathe + 0.05f * t2, 0.40f, 0.68f));
  if (ui.wetBase > 0.0f)
    g_reverb.setWet(
        clampf(ui.wetBase * (0.85f + 0.35f * breathe) + 0.03f * t3,
               0.10f, 0.60f));
  // barely darker in silence, tide-drifted around the player's base —
  // an earlier 15% dip stacked with the aging tape into gloom
  const float ratio = (0.95f + 0.15f * t3) * (1.0f - 0.07f * breathe);
  g_engine.setParam(Param::FilterCutoffHz,
                    clampf(ui.cutoff * ratio, 300.0f, 12000.0f));

  // background voting (default set only — a hand-picked chord is law):
  // every few minutes the fifth quietly becomes a 7/4 and back, with an
  // ADSR crossfade via a fresh note id
  if (!g_bgCustom && g_tampura && g_bgCount >= 2) {
    int want = g_weather.fifthVariant;
    if (t2 > 0.6f) want = 1;
    else if (t2 < -0.6f) want = 0;
    if (want != g_weather.fifthVariant) {
      g_weather.fifthVariant = want;
      g_engine.noteOff(g_bg[1].id);
      g_bg[1].cents = want ? 968.8f : 702.0f;
      g_bg[1].id = 1000 + (g_bgNextId++ & 15);
      g_engine.noteOn(g_bg[1].id, g_bg[1].cents, want ? 0.14f : 0.16f);
    }
  }
}

float rnd01f() { return (float)(rand() % 10000) * 0.0001f; }

void gardenPush(float cents) {
  g_garden.ring[g_garden.head] = {cents, nowSec()};
  g_garden.head = (g_garden.head + 1) % GhostGarden::kCap;
  if (g_garden.count < GhostGarden::kCap) ++g_garden.count;
}

void gardenSilenceGhosts() {
  if (!g_garden.activeMask) return;
  for (int i = 0; i < 4; ++i)
    if (g_garden.activeMask & (1u << i)) g_engine.noteOff(kGhostIdBase + i);
  g_garden.activeMask = 0;
}

double expDelay(double meanSec) {
  return -meanSec * log(1.0 - 0.9999 * (double)rnd01f());
}

void gardenTick(const Ui& ui) {
  const double now = nowSec();
  if (now - g_garden.lastInput < kGhostIdleSec || g_garden.count == 0) {
    g_garden.nextAt = 0.0;
    return;
  }
  if (g_garden.nextAt == 0.0) {
    g_garden.nextAt = now + 2.0 + expDelay(9.0);  // first ghost comes sooner
    return;
  }
  if (now < g_garden.nextAt) return;
  g_garden.nextAt = now + expDelay(12.0);

  // pick a remembered note, biased toward the fresh end of the ring
  const float u = rnd01f();
  const int back = (int)(u * u * (float)(g_garden.count - 1));
  const int idx =
      (g_garden.head - 1 - back + 2 * GhostGarden::kCap) % GhostGarden::kCap;
  float cents = g_garden.ring[idx].cents;
  const float r = rnd01f();
  if (r < 0.40f) { /* as played */ }
  else if (r < 0.70f) cents += 1200.0f;  // octave up
  else if (r < 0.85f) cents += 702.0f;   // pure fifth
  else cents -= 498.0f;                  // fourth below
  const float vel = 0.20f + 0.15f * rnd01f();

  const int slot = g_garden.nextSlot++ & 3;
  const int32_t id = kGhostIdBase + slot;
  g_garden.activeMask |= (1u << slot);
  // soft attack just for this note — the engine's event queue is FIFO and
  // fully drained before each render, so this sequence is race-free
  g_engine.setParam(Param::EnvAttack, 1.2f);
  g_engine.noteOn(id, cents, vel);
  g_engine.setParam(Param::EnvAttack, timbrePreset(ui.preset).attack);
  const double hold = 2.0 + 2.5 * (double)rnd01f();
  schedule(hold, [id] { g_engine.noteOff(id); });
  printf("\n  ~ duch: %+.0f c\n", (double)cents);
}

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
  printf("\r\x1b[K[%s] %s %.0fHz latch:%s flt:%.0fHz voices:%d",
         scaleInfo((ScaleId)ui.scale).name, kPresetNames[ui.preset],
         kBaseOctaves[ui.octave], ui.latch ? "ON " : "off", ui.cutoff,
         g_engine.activeVoiceCount());
  const Looper::State ls = g_looper.state();
  if (ls == Looper::State::Empty) {
    printf(" loop:--");
  } else if (ls == Looper::State::RecordFirst) {
    printf(" loop:REC %.1fs", (float)g_looper.lengthFrames() / kSr);
  } else {
    printf(" loop:%s %dlyr undo:%d %.1f/%.1fs vol:%.0f%%", loopStateName(ls),
           g_looper.layerCount(), g_looper.undoCount(),
           (float)g_looper.positionFrames() / kSr,
           (float)g_looper.lengthFrames() / kSr,
           g_looper.playbackLevel() * 100.0f);
  }
  if (!g_echo.enabled()) printf(" echo:OFF");
  if (!g_tampura) printf(" tamb:OFF");
  if (!g_strings.enabled()) printf(" str:OFF");
  if (g_reverb.wet() <= 0.0f) printf(" rev:OFF");
  if (g_pickMode) printf(" [WYBOR TLA %d/4]", g_bgCount);
  if (g_recOn.load(std::memory_order_relaxed))
    printf(" REC:%.0fs", (double)g_recIdx.load() / kSr);
  if (g_sim.inFlight) {
    printf("  LOT %.2fs spin:%+d", nowSec() - g_sim.t0, g_sim.spin);
  } else if (g_sim.centerCents != 0.0f) {
    printf("  centrum:%+.0fc tempo:%.2fx", g_sim.centerCents, g_sim.rateBase);
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

// Non-blocking single-byte read with a small timeout (for ESC sequences).
int readByte(int timeoutMs) {
  for (int waited = 0;; waited += 2) {
    char c = 0;
    if (read(STDIN_FILENO, &c, 1) == 1) return (unsigned char)c;
    if (waited >= timeoutMs) return -1;
    sleepMs(2);
  }
}

// --- toss mechanics ---
// Altitude curve: cents of lift as a function of flight time.
float flightAltCents(double t) {
  float a = (float)(620.0 * t);
  return a > 1250.0f ? 1250.0f : a;
}

void flightUpdate(const Ui& ui) {
  const double t = nowSec() - g_sim.t0;
  const float alt = flightAltCents(t);
  // spin is audible in flight: a pitch warble, faster/wider with more flips
  const float wob =
      (float)g_sim.spin * 14.0f * sinf((float)(2.0 * 3.14159265 * 5.5 * t));
  g_engine.setParam(Param::BendCents, alt + wob);
  g_looper.setRate(g_sim.rateBase * exp2f(alt / 1200.0f));
  (void)ui;
}

void land(Ui& ui) {
  const double t = nowSec() - g_sim.t0;
  g_sim.inFlight = false;
  if (t < 0.15) {  // too short to count as a throw
    g_engine.setParam(Param::BendCents, 0.0f);
    g_looper.setRate(g_sim.rateBase);
    printf("\n  >> za krotki lot — nic sie nie stalo\n");
    return;
  }
  int rung = (int)(t / 0.38) + 1;
  if (rung > kNumRungs) rung = kNumRungs;
  const float dir = g_sim.spin < 0 ? -1.0f : 1.0f;
  const float landCents = dir * kRungCents[rung - 1];
  g_sim.centerCents = clampf(g_sim.centerCents + landCents, -2400.0f, 2400.0f);
  applyCenter(ui);
  g_sim.rateBase = clampf(g_sim.rateBase * exp2f(landCents / 1200.0f),
                          0.25f, 4.0f);
  g_looper.setRate(g_sim.rateBase);
  g_engine.setParam(Param::BendCents, 0.0f);

  // dizziness: the more spin, the more the landing wobbles before settling
  const int flips = g_sim.spin < 0 ? -g_sim.spin : g_sim.spin;
  const int wobbles = flips > 4 ? 4 : flips;
  for (int k = 1; k <= wobbles; ++k) {
    const float amp = 70.0f / (float)k * (k % 2 == 0 ? 1.0f : -1.0f);
    schedule(0.10 * k, [amp] { g_engine.setParam(Param::BendCents, amp); });
  }
  if (wobbles > 0)
    schedule(0.10 * (wobbles + 1),
             [] { g_engine.setParam(Param::BendCents, 0.0f); });

  printf("\n  >> ladowanie %s%s (%d flip%s) -> centrum %+.0f c\n",
         dir < 0 ? "-" : "+", kRungNames[rung - 1], flips,
         flips == 1 ? "" : "y", (double)g_sim.centerCents);
}

void fumble() {
  const bool wasFlying = g_sim.inFlight;
  g_sim.inFlight = false;
  // a comic stumble — the tape trips and gets back up; NOTHING is lost.
  // (An earlier version dropped the top layer here: punishing the player
  // fights the whole point of the instrument — every gesture should be safe.)
  g_engine.setParam(Param::BendCents, -350.0f);
  g_looper.setRate(clampf(g_sim.rateBase * 0.3f, 0.1f, 4.0f));
  schedule(0.35, [] {
    g_engine.setParam(Param::BendCents, 0.0f);
    g_looper.setRate(g_sim.rateBase);
  });
  printf("\n  >> FUSZERKA%s — taśma sie potkna, muzyka przezywa\n",
         wasFlying ? " W LOCIE" : "");
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
  srand((unsigned)time(nullptr));
  g_engine.init(kSr);
  g_engine.setParam(Param::TimbrePreset, 3);  // CHIME
  g_engine.setParam(Param::BaseHz, 220.0f);
  g_engine.setParam(Param::FilterCutoffHz, 7500.0f);
  g_reverb.init(kSr);
  g_reverb.setWet(0.35f);
  g_strings.init((float)kSr);
  g_strings.setRootHz(220.0f);

  // Loop storage lives for the whole run; allocated before audio starts.
  // 8 undo levels: the host has RAM to spare, and the fumble gesture eats
  // one level per crash — a single level felt broken in play-testing.
  constexpr int kUndoDepth = 8;
  static std::vector<int16_t> mixBuf((size_t)kMaxLoopSec * kSr);
  static std::vector<int16_t> layerBuf((size_t)kMaxLoopSec * kSr);
  static std::vector<int16_t> undoBuf((size_t)kUndoDepth * kMaxLoopSec * kSr);
  g_looper.init(mixBuf.data(), layerBuf.data(), undoBuf.data(), kUndoDepth,
                (uint32_t)mixBuf.size());
  g_looper.setPlaybackLevel(0.8f);  // leave headroom to play over the loop

  static std::vector<int16_t> echoBuf((size_t)(kEchoSec * kSr));
  g_echoStorage = echoBuf.data();
  g_echoFrames = (uint32_t)echoBuf.size();
  g_echo.init(echoBuf.data(), (uint32_t)echoBuf.size(), (float)kSr);

  // session recorder: up to 10 minutes of the final mix, armed with SHIFT+W
  static std::vector<int16_t> recBuf((size_t)10 * 60 * kSr);
  g_recBuf = &recBuf;
  g_echo.setFeedback(0.55f);  // each return ~5 dB quieter, ~4 audible repeats
  g_echo.setLevel(0.5f);

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
  raw.c_cc[VTIME] = 0;  // fully non-blocking; the sim needs a fast tick
  tcsetattr(STDIN_FILENO, TCSANOW, &raw);

  printf("grajek_live — engine at %d Hz, ~16 ms latency\n", kSr);
  printf("  Domyslnie gra WSZYSTKO, co robi ladnie: barwa CHIME, poglos,\n"
         "  tampura (cichy dron pod spodem) i echo %.0f s. Klikaj pojedyncze\n"
         "  nuty i sluchaj. SHIFT+E echo, SHIFT+T tampura, SHIFT+V poglos.\n",
         (double)kEchoSec);
  printf("  grid: keyboard rows = instrument rows (bottom = lowest)\n");
  printf("  TAB scale | ` timbre | BACKSPACE octave | SHIFT+L latch (drony!)\n");
  printf("  arrows <- -> bend | up/down filter | ENTER panic+re-center | ESC quit\n");
  printf("  FREEZE: SHIFT+F — to co teraz slychac staje sie PODLOGA (petla\n"
         "          wstecz z pamieci echa; lata z rzutami, SHIFT+U cofa)\n");
  printf("  DUCHY:  po ~12 s ciszy pudelko cicho dosiewa twoje wlasne nuty;\n"
         "          pierwszy klawisz je ucisza | SHIFT+W nagrywa sesje do WAV\n");
  printf("  TLO:    SHIFT+D i klikasz nuty = wybierasz wlasny dron pod spodem\n"
         "          (max 4, klik drugi raz usuwa) | SHIFT+S struny sympatyczne\n");
  printf("  LOOPER (zaawansowane): SHIFT+R rec/close/overdub | SHIFT+P"
         " play/stop\n"
         "          SHIFT+U undo | SHIFT+C clear | SHIFT+[ ] volume"
         " (max %ds)\n", kMaxLoopSec);
  printf("  RZUT:   SPACE = wyrzut ... SPACE = chwyt (czas lotu -> szczebel JI)\n"
         "          strzalki W LOCIE = spin (-> w gore / <- w dol, wiecej = zawrot)\n"
         "          SHIFT+X = fuszerka (muzyka upada, tracisz warstwe)\n\n");

  Ui ui;
  g_weather.start = nowSec();
  backgroundApply();
  printStatus(ui);
  double lastStatusAt = nowSec();

  while (g_running) {
    const int b = readByte(0);

    if (b >= 0) {
      const char c = (char)b;
      KeyPos kp;
      // any human input marks presence: sowing pauses, sounding ghosts bow out
      g_garden.lastInput = nowSec();
      gardenSilenceGhosts();
      if (c == 0x1B) {  // ESC alone, or an escape sequence
        const int c2 = readByte(40);
        if (c2 < 0) {
          g_running = 0;  // nothing followed: bare ESC = quit
        } else if (c2 == '[') {
          const int c3 = readByte(40);
          if (g_sim.inFlight) {
            // in flight the arrows are SPIN, not bend/filter
            if (c3 == 'C') g_sim.spin += 1;
            if (c3 == 'D') g_sim.spin -= 1;
          } else {
            switch (c3) {
              // cutoff only updates the player's base — the weather tick is
              // the single writer of the actual engine parameter
              case 'A': ui.cutoff = clampf(ui.cutoff * 1.15f, 200.0f, 12000.0f); break;
              case 'B': ui.cutoff = clampf(ui.cutoff / 1.15f, 200.0f, 12000.0f); break;
              case 'C': ui.bend = clampf(ui.bend + 25.0f, -300.0f, 300.0f); break;
              case 'D': ui.bend = clampf(ui.bend - 25.0f, -300.0f, 300.0f); break;
              default: break;
            }
            g_engine.setParam(Param::BendCents, ui.bend);
          }
        }
        // any other sequence (Alt+key, F-keys): ignore, keep playing
      } else if (c == ' ') {
        if (!g_sim.inFlight) {
          g_sim.inFlight = true;
          g_sim.t0 = nowSec();
          g_sim.spin = 0;
        } else {
          land(ui);
        }
      } else if (c == 'X') {
        fumble();
      } else if (c == '\t') {
        ui.scale = (ui.scale + 1) % (int)ScaleId::Count;
        allOff();  // clean retuning
        backgroundApply();
      } else if (c == '`') {
        ui.preset = (ui.preset + 1) % kNumTimbrePresets;
        g_engine.setParam(Param::TimbrePreset, (float)ui.preset);
      } else if (c == 0x7F || c == 0x08) {  // backspace
        ui.octave = (ui.octave + 1) % 4;
        applyCenter(ui);
      } else if (c == 'L') {
        ui.latch = !ui.latch;
      } else if (c == 'E') {
        g_echo.setEnabled(!g_echo.enabled());
      } else if (c == 'T') {
        g_tampura = !g_tampura;
        backgroundApply();
      } else if (c == 'D') {
        g_pickMode = !g_pickMode;
        if (g_pickMode)
          printf("\n  >> WYBOR TLA: klikaj nuty siatki (toggle, max 4);"
                 " SHIFT+D konczy\n");
        else
          printf("\n  >> tlo ustawione (%d nut)\n", g_bgCount);
      } else if (c == 'S') {
        g_strings.setEnabled(!g_strings.enabled());
      } else if (c == 'V') {
        // cycle the room base: dry -> subtle -> default -> cathedral
        // (the weather breathes around this base)
        ui.wetBase = ui.wetBase <= 0.0f ? 0.2f
                     : (ui.wetBase <= 0.2f ? 0.35f
                        : (ui.wetBase <= 0.35f ? 0.55f : 0.0f));
        if (ui.wetBase <= 0.0f) g_reverb.setWet(0.0f);
      } else if (c == 'W') {
        if (!g_recOn.load(std::memory_order_relaxed)) {
          g_recIdx.store(0);
          g_recOn.store(true);
          printf("\n  >> REC: nagrywam sesje (SHIFT+W konczy i zapisuje)\n");
        } else {
          g_recOn.store(false);
          const uint32_t nrec = g_recIdx.load();
          char path[64];
          snprintf(path, sizeof path, "session_%ld.wav", (long)time(nullptr));
          if (nrec > 0 && g_recBuf &&
              writeWavMono16(path, g_recBuf->data(), nrec, kSr))
            printf("\n  >> zapisane: %s (%.1f s)\n", path, (double)nrec / kSr);
          else
            printf("\n  >> nic nie nagrano\n");
        }
      } else if (c == 'F') {
        // FREEZE: the last few seconds of the flowing memory crystallize
        // retroactively into a persistent floor (it varispeeds with tosses,
        // undoes like a layer); the echo then flows on above it. Capture
        // runs on the audio thread next block; the tape is cleared shortly
        // AFTER so the copy sees it intact.
        g_looper.requestCapture(g_echoStorage, g_echoFrames);
        schedule(0.08, [] { g_echo.clearTape(); });
        printf("\n  >> FREEZE — ostatnie %.0f s pamieci staje sie podloga\n",
               (double)kEchoSec);
      } else if (c == 'R') {
        g_looper.toggleRecord();
      } else if (c == 'P') {
        g_looper.togglePlay();
      } else if (c == 'U') {
        g_looper.undoLayer();
      } else if (c == 'C') {
        // full reset to zero: loop gone, echo tape wiped, flight cancelled,
        // home center, tape at 1x, bend neutral — nothing survives a clear
        g_looper.clear();
        g_echo.clearTape();
        g_pending.clear();
        g_sim.inFlight = false;
        g_sim.centerCents = 0.0f;
        g_sim.rateBase = 1.0f;
        g_looper.setRate(1.0f);
        ui.bend = 0.0f;
        g_engine.setParam(Param::BendCents, 0.0f);
        applyCenter(ui);
      } else if (c == '{') {
        g_looper.setPlaybackLevel(g_looper.playbackLevel() - 0.1f);
      } else if (c == '}') {
        g_looper.setPlaybackLevel(g_looper.playbackLevel() + 0.1f);
      } else if (c == '\r' || c == '\n') {
        // panic: silence + neutral pitch + back to home center; loop keeps going
        allOff();
        g_pending.clear();
        ui.bend = 0.0f;
        g_sim.inFlight = false;
        g_sim.centerCents = 0.0f;
        g_sim.rateBase = 1.0f;
        g_engine.setParam(Param::BendCents, 0.0f);
        g_looper.setRate(1.0f);
        applyCenter(ui);
        backgroundApply();
      } else if (keyToGrid(c, kp)) {
        const int id = kp.row * 14 + kp.col;
        const float cents = gridToCents((ScaleId)ui.scale, kp.col, kp.row);
        if (g_pickMode) {
          // in pick mode the grid edits the background chord instead of playing
          backgroundToggleNote(cents);
          printStatus(ui);
          lastStatusAt = nowSec();
          continue;
        }
        // humanized dynamics — identical velocities sound mechanical
        const float vel = 0.55f + 0.35f * (float)(rand() % 1000) / 1000.0f;
        NoteState& st = g_notes[id];
        if (ui.latch) {
          if (st.on) {
            g_engine.noteOff(id);
            st = NoteState{};
          } else {
            g_engine.noteOn(id, cents, vel);
            gardenPush(cents);
            st.on = true;
            st.latched = true;
          }
        } else {
          // press/auto-repeat plays and extends; fades out 0.6 s later
          g_engine.noteOn(id, cents, vel);
          if (!st.on) gardenPush(cents);  // remember presses, not auto-repeats
          st.on = true;
          st.latched = false;
          st.offAt = nowSec() + 0.6;
        }
      }
      printStatus(ui);
      lastStatusAt = nowSec();
    }

    if (g_sim.inFlight) flightUpdate(ui);
    runPending();
    weatherTick(ui);
    gardenTick(ui);

    // keep the loop position / flight state on screen while idling
    const bool busy =
        g_sim.inFlight || g_looper.state() != Looper::State::Empty;
    if (busy && nowSec() - lastStatusAt > (g_sim.inFlight ? 0.1 : 0.25)) {
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

    sleepMs(8);  // ~125 Hz tick: smooth flight, negligible input latency
  }

  allOff();
  sleepMs(150);  // let the release tail ring before closing the queue
  AudioQueueStop(queue, true);
  AudioQueueDispose(queue, true);
  tcsetattr(STDIN_FILENO, TCSANOW, &orig);
  printf("\nbye!\n");
  return 0;
}
