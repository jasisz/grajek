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
//           SHIFT+, / SHIFT+. = shake a remembered phrase down / up
//           ENTER panic (silence + bend + re-center; keeps the loop) | ESC quit
//
// Looper (records what you play and layers on top; currently host-only — the
// device transport is deliberately deferred until it earns a gesture):
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
//           SHIFT+X = fumble: a comic stumble — the tape dives and gets back
//                 up, nothing is lost; during flight = crash landing
#include <AudioToolbox/AudioToolbox.h>
#include <signal.h>
#include <termios.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <vector>

#include <stdlib.h>

#include "ga_echo.h"
#include "ga_chorus.h"
#include "ga_engine.h"
#include "ga_looper.h"
#include "ga_reverb.h"
#include "ga_scales.h"
#include "ga_strings.h"
#include "gk_garden.h"
#include "gk_pulse.h"
#include "wav_writer.h"

using namespace ga;

namespace {

constexpr int kSr = 48000;
constexpr int kFramesPerBuf = 256;  // ~5.3 ms per buffer
constexpr int kNumBufs = 3;         // ~16 ms total output latency

Engine g_engine;
Looper g_looper;
Chorus g_chorus;
EchoTape g_echo;   // always-on ambient memory: play anything, it comes back
Reverb g_reverb;   // the single biggest "sounds pretty by itself" ingredient
constexpr int kMaxLoopSec = 60;
constexpr float kEchoSec = 4.0f;

// Reich drift — a personal dedication wired into the architecture: the loop
// runs 0.25% fast, so a frozen floor slowly creeps against your pulse and
// the echo's memory, "It's Gonna Rain"-style. Inaudible at first, audible
// over minutes. Set to 1.0f to turn the homage off.
constexpr float kReichDrift = 1.0025f;
void setLoopRate(float r) { g_looper.setRate(r * kReichDrift); }

bool g_harmony = false;  // SHIFT+H: a quiet harmonic shadow one row above

// the echo's raw tape, shared with the FREEZE capture
int16_t* g_echoStorage = nullptr;
uint32_t g_echoFrames = 0;

// The background (tampura): a quiet pedal under everything — random notes
// over a drone sound intentional. Default is 1/1 + 3/2; the player can PICK
// their own chord (up to 4 notes) in pick mode (SHIFT+D here; firmware keeps
// the hook, while the current device UI only toggles its default background).
// Follows toss modulations automatically (voices track BaseHz live).
bool g_tampura = true;
bool g_pickMode = false;
struct BgNote { float cents; int32_t id; };
BgNote g_bg[4] = {{0.0f, 1000}, {702.0f, 1001}, {0.0f, -1}, {0.0f, -1}};
int g_bgCount = 2;
bool g_bgCustom = false;   // a hand-picked background silences the weather vote
int32_t g_bgNextId = 2;    // rotates through ids 1000..1015

// The background has its OWN timbre (ORGAN), independent of what the player
// plays with — a percussive preset (MUSICBOX, sustain 0) used to silently
// kill the drone, which made picking a background look broken. The engine
// Per-note preset events keep this colour independent from the player's
// global bus/filter preset.
constexpr int kBgPreset = 1;  // ORGAN

void bgNoteOn(int32_t id, float cents, float vel) {
  g_engine.noteOnPersistentWithPreset(id, cents, vel, kBgPreset);
}

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

// Ghost garden: minutes-scale memory. It keeps short gestures, not a sack of
// unrelated pitches: phrase boundaries plus the player's inter-onset timing.
// After ~7 s of silence the box quietly re-sows one whole recent phrase. The
// player's first real key silences the ghosts immediately — it never fights
// the human.
using GardenPhrase = gk::GardenPhrase;

struct GardenReplay {
  GardenPhrase phrase;
  int pos = 0;
  double nextAt = 0.0;
  float transpose = 0.0f;
  float velocity = 0.0f;
  bool active = false;
};

struct GardenNoteOff {
  double at = 0.0;
  int32_t id = -1;
};

struct GhostPlayback {
  double lastInput = 0.0;
  double nextAt = 0.0;
  uint32_t activeMask = 0;  // which of the 4 ghost ids are sounding
  int nextSlot = 0;
};
gk::Garden g_garden;
GhostPlayback g_ghost;
GardenReplay g_ghostPhrase;
GardenReplay g_shakePhrase;
GardenNoteOff g_ghostOffs[6];
GardenNoteOff g_shakeOffs[8];
int g_ghostOffCount = 0;
int g_shakeOffCount = 0;
uint32_t g_shakeMask = 0;
int g_shakeSlot = 0;
int g_shakeStep = 1;
constexpr int32_t kGhostIdBase = 1100;
constexpr int32_t kShakeIdBase = 1120;
// 7 s, synced with the device: 12 s made the box's memory practically
// inaudible in normal play (a child does not pause that long)
constexpr double kGhostIdleSec = 7.0;
// Cat, not Furby: the box never solicits play. Ghosts may only CONTINUE a
// session the human started — with soul-loaded memories the garden is full
// at boot, and without this gate it would start singing on its own.
bool g_playedThisSession = false;

// Pulse entrainment: the box never imposes a tempo — it follows yours.
// Regular playing (a few even intervals) summons a quiet MUSICBOX heartbeat
// on the root, phase-anchored to YOUR presses; stop, or drift into rubato,
// and it lets go within a few beats. The ghosts remember the last pulse.
gk::PulseTracker g_pulse;

// Session recorder: the final mix (post-everything), armed with SHIFT+W.
std::atomic<bool> g_recOn{false};
std::atomic<uint32_t> g_recIdx{0};
std::vector<int16_t>* g_recBuf = nullptr;

void backgroundApply() {
  for (int i = 0; i < g_bgCount; ++i) {
    if (g_tampura)
      bgNoteOn(g_bg[i].id, g_bg[i].cents, i == 0 ? 0.22f : 0.16f);
    else
      g_engine.noteOff(g_bg[i].id);
  }
}

int32_t nextBackgroundId() {
  // IDs are reusable only after their old note has bowed out. A blind
  // modulo-16 rotation eventually landed on an active root and a later
  // noteOff for the changing fifth could cut that root too.
  for (int attempt = 0; attempt < 16; ++attempt) {
    const int32_t id = 1000 + (g_bgNextId++ & 15);
    bool used = false;
    for (int i = 0; i < g_bgCount; ++i)
      if (g_bg[i].id == id) used = true;
    if (!used) return id;
  }
  return -1;  // impossible with a four-note background and sixteen IDs
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
  const int32_t id = nextBackgroundId();
  if (id < 0) return;
  g_bg[g_bgCount] = {cents, id};
  ++g_bgCount;
  if (g_tampura) bgNoteOn(id, cents, g_bgCount == 1 ? 0.22f : 0.16f);
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
  g_chorus.process(samples, frames);
  // Generosity layer: everything played joins a fading ambient memory.
  g_echo.process(samples, samples, frames);
  // The manual looper records synth+ambience and adds loop playback on top.
  g_looper.process(samples, samples, frames);
  // Reverb last, so the loop and echo sit in the same room.
  g_reverb.process(samples, frames);
  // final glue clip — stacked layers can sum hot; keep the edge soft
  // (device adds a stricter child-safe output ceiling on top of this)
  for (int i = 0; i < frames; ++i)
    samples[i] = softClip(samples[i]) * 0.85f;
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

uint32_t nowMs32() {
  using namespace std::chrono;
  // Integer-to-unsigned conversion is defined modulo 2^32. Going through the
  // old double-seconds adapter became undefined once host uptime exceeded the
  // 49.7-day uint32 millisecond range.
  return static_cast<uint32_t>(
      duration_cast<milliseconds>(steady_clock::now().time_since_epoch())
          .count());
}

const char* kPresetNames[kNumTimbrePresets] = {"PURE", "ORGAN", "REED",
                                               "CHIME", "MUSICBOX", "WARM",
                                               "HOLLOW"};
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
  g_echo.setRhythmicTap((float)g_pulse.periodSec(), g_pulse.ticking());
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
      const int32_t id = nextBackgroundId();
      if (id >= 0) {
        g_bg[1].id = id;
        bgNoteOn(id, g_bg[1].cents, want ? 0.14f : 0.16f);
      }
    }
  }
}

float rnd01f() { return (float)(rand() % 10000) * 0.0001f; }

float hostGardenRandom(void*) { return rnd01f(); }

uint16_t replayGapMs(uint16_t recordedMs) {
  return gk::Garden::replayGapMs(recordedMs);
}

bool selectGardenPhrase(GardenPhrase* out) {
  return g_garden.selectPhrase(out, hostGardenRandom, nullptr);
}

void gardenPush(float cents) {
  const double now = nowSec();
  g_playedThisSession = true;
  g_garden.push(cents, nowMs32());
  g_ghost.lastInput = now;
}

void pulseOnOnset() {
  g_pulse.onOnset(nowSec());
}

void pulseTick() {
  const int chordCount = g_tampura ? g_bgCount : 0;
  const gk::PulseBeat beat = g_pulse.tick(nowSec(), chordCount);
  if (!beat.play) return;
  const float cents = beat.chordIndex >= 0 ? g_bg[beat.chordIndex].cents : 0.0f;
  // MUSICBOX ends by itself — no note-off bookkeeping
  g_engine.noteOnWithPreset(1300, cents, beat.velocity, 4);
}

double expDelay(double meanSec) {
  return -meanSec * log(1.0 - 0.9999 * (double)rnd01f());
}

void drainGardenOffs(GardenNoteOff* offs, int& count, int32_t idBase,
                     int slots, uint32_t& activeMask, double now) {
  for (int i = 0; i < count;) {
    if (offs[i].at <= now) {
      g_engine.noteOff(offs[i].id);
      const int slot = (int)(offs[i].id - idBase);
      if (slot >= 0 && slot < slots) activeMask &= ~(1u << slot);
      offs[i] = offs[--count];
    } else {
      ++i;
    }
  }
}

void drainGardenOffs() {
  const double now = nowSec();
  drainGardenOffs(g_ghostOffs, g_ghostOffCount, kGhostIdBase, 4,
                  g_ghost.activeMask, now);
  drainGardenOffs(g_shakeOffs, g_shakeOffCount, kShakeIdBase, 8,
                  g_shakeMask, now);
}

void gardenSilenceGhosts() {
  g_ghostPhrase.active = false;
  g_ghost.nextAt = 0.0;
  for (int i = 0; i < g_ghostOffCount; ++i)
    g_engine.noteOff(g_ghostOffs[i].id);
  g_ghostOffCount = 0;
  for (int i = 0; i < 4; ++i)
    if (g_ghost.activeMask & (1u << i)) g_engine.noteOff(kGhostIdBase + i);
  g_ghost.activeMask = 0;
}

void gardenSilenceShake() {
  g_shakePhrase.active = false;
  for (int i = 0; i < g_shakeOffCount; ++i)
    g_engine.noteOff(g_shakeOffs[i].id);
  g_shakeOffCount = 0;
  for (int i = 0; i < 8; ++i)
    if (g_shakeMask & (1u << i)) g_engine.noteOff(kShakeIdBase + i);
  g_shakeMask = 0;
}

void scheduleNextGhost(double now, bool firstAfterSilence) {
  const double delay = firstAfterSilence ? 1.5 + expDelay(6.0)
                                         : 0.8 + expDelay(9.0);
  double target = now + delay;
  // Only the phrase start snaps to the remembered pulse. The internal
  // timing remains the player's own instead of quantizing every note.
  const double period = g_pulse.memoryPeriodSec();
  if (period > 0.15) {
    const double anchor = g_pulse.lastOnsetSec();
    const double k = round((target - anchor) / period);
    target = anchor + k * period;
    while (target < now + 0.2) target += period;
  }
  g_ghost.nextAt = target;
}

void ghostPhraseStart(double now) {
  GardenPhrase phrase;
  if (!selectGardenPhrase(&phrase)) return;
  gardenSilenceGhosts();
  g_ghostPhrase.phrase = phrase;
  g_ghostPhrase.pos = 0;
  g_ghostPhrase.nextAt = now;
  g_ghostPhrase.velocity = 0.25f + 0.11f * rnd01f();
  const float r = rnd01f();
  if (r < 0.75f) g_ghostPhrase.transpose = 0.0f;
  else if (r < 0.84f) g_ghostPhrase.transpose = 1200.0f;
  else if (r < 0.92f) g_ghostPhrase.transpose = 702.0f;
  else g_ghostPhrase.transpose = -498.0f;
  g_ghostPhrase.active = true;
  printf("\n  ~ duch-fraza: %d nut, przesuniecie %+.0f c\n", phrase.count,
         (double)g_ghostPhrase.transpose);
}

void ghostPhraseTick(const Ui& ui, double now) {
  if (!g_ghostPhrase.active || now < g_ghostPhrase.nextAt) return;
  const int i = g_ghostPhrase.pos;
  const bool more = i + 1 < g_ghostPhrase.phrase.count;
  const uint16_t nextGap =
      more ? replayGapMs(g_ghostPhrase.phrase.note[i + 1].gapMs) : 0;
  const double hold = more ? std::min(1.0, (nextGap + 180) * 0.001) : 1.4;
  const int slot = g_ghost.nextSlot++ & 3;
  const int32_t id = kGhostIdBase + slot;
  g_ghost.activeMask |= (1u << slot);
  g_engine.noteOnWithPreset(
      id, g_ghostPhrase.phrase.note[i].cents + g_ghostPhrase.transpose,
      g_ghostPhrase.velocity * (i == 0 ? 1.0f : 0.88f), ui.preset, 0.22f);
  if (g_ghostOffCount < (int)(sizeof g_ghostOffs / sizeof g_ghostOffs[0]))
    g_ghostOffs[g_ghostOffCount++] = {now + hold, id};

  ++g_ghostPhrase.pos;
  if (more) {
    g_ghostPhrase.nextAt = now + nextGap * 0.001;
  } else {
    g_ghostPhrase.active = false;
    scheduleNextGhost(now, false);
  }
}

void gardenTick(const Ui& ui) {
  if (!g_playedThisSession) return;  // ghosts continue sessions, never start them
  const double now = nowSec();
  if (now - g_ghost.lastInput < kGhostIdleSec || g_garden.count() == 0) {
    if (g_ghostPhrase.active || g_ghost.activeMask || g_ghostOffCount)
      gardenSilenceGhosts();
    else
      g_ghost.nextAt = 0.0;
    return;
  }
  if (g_ghostPhrase.active) {
    ghostPhraseTick(ui, now);
    return;
  }
  if (g_ghost.nextAt == 0.0) {
    scheduleNextGhost(now, true);
    return;
  }
  if (now < g_ghost.nextAt) return;
  ghostPhraseStart(now);
  ghostPhraseTick(ui, now);
}

void shakePhraseTick(const Ui& ui, double now) {
  if (!g_shakePhrase.active || now < g_shakePhrase.nextAt) return;
  const int i = g_shakePhrase.pos;
  const int slot = g_shakeSlot++ & 7;
  const int32_t id = kShakeIdBase + slot;
  g_shakeMask |= (1u << slot);
  g_engine.noteOnWithPreset(
      id, g_shakePhrase.phrase.note[i].cents + g_shakePhrase.transpose,
      g_shakePhrase.velocity * (i == 0 ? 1.0f : 0.88f), ui.preset, 0.06f);
  if (g_shakeOffCount < (int)(sizeof g_shakeOffs / sizeof g_shakeOffs[0]))
    g_shakeOffs[g_shakeOffCount++] = {now + 0.42, id};

  ++g_shakePhrase.pos;
  if (g_shakePhrase.pos >= g_shakePhrase.phrase.count) {
    g_shakePhrase.active = false;
  } else {
    const uint16_t gap =
        replayGapMs(g_shakePhrase.phrase.note[g_shakePhrase.pos].gapMs);
    g_shakePhrase.nextAt = now + gap * 0.001;
  }
}

void gardenShake(const Ui& ui, float dir) {
  const double now = nowSec();
  g_playedThisSession = true;
  g_ghost.lastInput = now;
  gardenSilenceGhosts();
  if (g_shakePhrase.active) return;  // let the remembered sentence finish
  gardenSilenceShake();              // release the previous sentence's tail

  GardenPhrase phrase;
  if (!g_garden.pluck(&phrase, dir, hostGardenRandom, nullptr)) {
    const int hi = 2 * scaleStepsPerOctave((ScaleId)ui.scale);
    g_shakeStep += dir >= 0.0f ? 1 : -1;
    g_shakeStep = std::max(1, std::min(g_shakeStep, hi - 1));
    phrase.count = 1;
    phrase.note[0] = {
        scaleStepCents((ScaleId)ui.scale, g_shakeStep), 0};
  }
  g_shakePhrase.phrase = phrase;
  g_shakePhrase.pos = 0;
  g_shakePhrase.nextAt = now;
  g_shakePhrase.transpose = 0.0f;  // gk::Garden::pluck already seasoned it
  g_shakePhrase.velocity = 0.70f;
  g_shakePhrase.active = true;
  printf("\n  >> wiatr wspomnien: %d nut, kierunek %s\n", phrase.count,
         dir >= 0.0f ? "w gore" : "w dol");
  shakePhraseTick(ui, now);
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
  if (g_pulse.ticking())
    printf(" puls:%d", (int)(60.0 / g_pulse.periodSec() + 0.5));
  if (g_harmony) printf(" harm:ON");
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
  gardenSilenceGhosts();
  gardenSilenceShake();
  g_engine.allNotesOff();
  for (auto& n : g_notes) n = NoteState{};
}

// --- the soul: grajek remembers you across sessions ---
// Saved on exit, loaded on start: your scale, timbre, octave, room, the
// background chord you picked and the ghost garden's memories. The box you
// taught yesterday is the box you pick up today.
const char* kSoulPath = "grajek_soul.txt";

void soulSave(const Ui& ui) {
  // Unique same-directory temp: concurrent players may race, but each rename
  // still installs one whole snapshot rather than sharing a writable inode.
  char tmpPath[] = "grajek_soul.txt.tmp.XXXXXX";
  const int tmpFd = mkstemp(tmpPath);
  FILE* f = tmpFd >= 0 ? fdopen(tmpFd, "w") : nullptr;
  if (!f) {
    if (tmpFd >= 0) close(tmpFd);
    unlink(tmpPath);
    fprintf(stderr, "Nie moge zapisac tymczasowej duszy.\n");
    return;
  }
  bool ok = fprintf(f, "grajek-soul 2\n") >= 0;
  ok = fprintf(f, "scale %d preset %d octave %d wet %.3f\n", ui.scale,
               ui.preset, ui.octave, (double)ui.wetBase) >= 0 && ok;
  ok = fprintf(f, "bg %d %d", g_bgCustom ? 1 : 0, g_bgCount) >= 0 && ok;
  for (int i = 0; i < g_bgCount; ++i)
    ok = fprintf(f, " %.2f", (double)g_bg[i].cents) >= 0 && ok;
  ok = fprintf(f, "\ngarden %d", g_garden.count()) >= 0 && ok;
  for (int i = 0; i < g_garden.count(); ++i) {
    ok = fprintf(f, " %.2f %u %d", (double)g_garden.cents(i),
                 (unsigned)g_garden.delayMs(i),
                 g_garden.startsPhrase(i) ? 1 : 0) >= 0 && ok;
  }
  ok = fprintf(f, "\n") >= 0 && ok;
  ok = fflush(f) == 0 && ok;
  ok = fsync(fileno(f)) == 0 && ok;
  ok = fclose(f) == 0 && ok;
  if (ok && rename(tmpPath, kSoulPath) == 0) return;
  unlink(tmpPath);
  fprintf(stderr, "Nie udalo sie bezpiecznie zapisac duszy.\n");
}

bool soulLoad(Ui& ui) {
  FILE* f = fopen(kSoulPath, "r");
  if (!f) return false;
  bool ok = false;
  char tag[32];
  int ver = 0, scale = 0, preset = 0, octave = 0, bgCustom = 0, bgCount = 0;
  float wet = 0.35f;
  Ui loadedUi = ui;
  BgNote loadedBg[4] = {};
  float loadedGardenCents[gk::kGardenCapacity] = {};
  uint16_t loadedGardenDelay[gk::kGardenCapacity] = {};
  uint32_t loadedPhraseStartMask = 0;
  int loadedGardenCount = 0;
  if (fscanf(f, "%31s %d", tag, &ver) == 2 &&
      strcmp(tag, "grajek-soul") == 0 && ver == 2 &&
      fscanf(f, " scale %d preset %d octave %d wet %f", &scale, &preset,
             &octave, &wet) == 4 &&
      fscanf(f, " bg %d %d", &bgCustom, &bgCount) == 2 &&
      scale >= 0 && scale < (int)ScaleId::Count && preset >= 0 &&
      preset < kNumTimbrePresets && octave >= 0 && octave < 4 &&
      std::isfinite(wet) && wet >= 0.0f && wet <= 0.6f &&
      (bgCustom == 0 || bgCustom == 1) && bgCount >= 0 && bgCount <= 4) {
    bool bgOk = true;
    for (int i = 0; bgOk && i < bgCount; ++i) {
      float cents = 0.0f;
      bgOk = fscanf(f, " %f", &cents) == 1 && std::isfinite(cents);
      if (bgOk) loadedBg[i] = {cents, 1000 + i};
    }
    int gc = -1;
    bool gardenOk = bgOk && fscanf(f, " garden %d", &gc) == 1 && gc >= 0 &&
                    gc <= gk::kGardenCapacity;
    for (int i = 0; gardenOk && i < gc; ++i) {
      float cents = 0.0f;
      unsigned gap = 0;
      int starts = 0;
      gardenOk = fscanf(f, " %f %u %d", &cents, &gap, &starts) == 3 &&
                 std::isfinite(cents) && gap <= 1499 &&
                 (starts == 0 || starts == 1) && (i != 0 || starts == 1);
      if (gardenOk) {
        const bool phraseStart = starts != 0;
        loadedGardenCents[i] = cents;
        loadedGardenDelay[i] = phraseStart ? (uint16_t)0 : (uint16_t)gap;
        if (phraseStart) loadedPhraseStartMask |= 1u << i;
      }
    }
    if (gardenOk) {
      loadedUi.scale = scale;
      loadedUi.preset = preset;
      loadedUi.octave = octave;
      loadedUi.wetBase = wet;
      loadedGardenCount = gc;
      ok = true;
    }
  }
  fclose(f);
  if (ok) {
    ui = loadedUi;
    for (int i = 0; i < bgCount; ++i) g_bg[i] = loadedBg[i];
    for (int i = bgCount; i < 4; ++i) g_bg[i] = {0.0f, -1};
    g_bgCount = bgCount;
    g_bgNextId = bgCount;
    g_bgCustom = bgCustom != 0;
    g_garden.restore(loadedGardenCents, loadedGardenDelay,
                     loadedPhraseStartMask, loadedGardenCount);
  }
  return ok;
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
  setLoopRate(g_sim.rateBase * exp2f(alt / 1200.0f));
  (void)ui;
}

void land(Ui& ui) {
  const double t = nowSec() - g_sim.t0;
  g_sim.inFlight = false;
  if (t < 0.15) {  // too short to count as a throw
    g_engine.setParam(Param::BendCents, 0.0f);
    setLoopRate(g_sim.rateBase);
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
  setLoopRate(g_sim.rateBase);
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
  setLoopRate(clampf(g_sim.rateBase * 0.3f, 0.1f, 4.0f));
  schedule(0.35, [] {
    g_engine.setParam(Param::BendCents, 0.0f);
    setLoopRate(g_sim.rateBase);
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
  g_chorus.init((float)kSr);
  g_chorus.setDepth(timbrePreset(3).chorusDepth);

  // Loop storage lives for the whole run; allocated before audio starts.
  // 8 undo levels: the host has RAM to spare, and a single level felt
  // broken in play-testing.
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
  printf("  SHIFT+B bas psychoakustyczny on/off (A/B na 55/110 Hz)\n");
  printf("  arrows <- -> bend | up/down filter | ENTER panic+re-center | ESC quit\n");
  printf("  FREEZE: SHIFT+F — to co teraz slychac staje sie PODLOGA (petla\n"
         "          wstecz z pamieci echa; lata z rzutami, SHIFT+U cofa)\n");
  printf("  OGROD:  po ~7 s ciszy pudelko dosiewa twoje krotkie frazy z rytmem;\n"
         "          SHIFT+, / SHIFT+. strzasa fraze w dol / w gore; pierwszy\n"
         "          klawisz ucisza wspomnienie | SHIFT+W nagrywa sesje do WAV\n");
  printf("  TLO:    SHIFT+D i klikasz nuty = wybierasz wlasny dron pod spodem\n"
         "          (max 4, klik drugi raz usuwa) | SHIFT+S struny sympatyczne\n");
  printf("  SHIFT+H cien harmoniczny (rzad wyzej gra cicho z toba — w PENTA\n"
         "          to tercje) | dusza: stan i wspomnienia w grajek_soul.txt\n");
  printf("  PULS:   graj rowno kilka nut, a dolaczy serce grajace ARPEGGIO\n"
         "          TWOJEGO TLA w twoim tempie; plynie za toba (tez na\n"
         "          polnuty/osemki), a gdy przestajesz — samo puszcza\n");
  printf("  LOOPER (zaawansowane): SHIFT+R rec/close/overdub | SHIFT+P"
         " play/stop\n"
         "          SHIFT+U undo | SHIFT+C clear | SHIFT+[ ] volume"
         " (max %ds)\n", kMaxLoopSec);
  printf("  RZUT:   SPACE = wyrzut ... SPACE = chwyt (czas lotu -> szczebel JI)\n"
         "          strzalki W LOCIE = spin (-> w gore / <- w dol, wiecej = zawrot)\n"
         "          SHIFT+X = fuszerka (muzyka sie potyka i wstaje, nic nie ginie)\n\n");

  Ui ui;
  const bool soulLoaded = soulLoad(ui);
  if (soulLoaded) {
    g_engine.setParam(Param::TimbrePreset, (float)ui.preset);
    g_chorus.setDepth(timbrePreset(ui.preset).chorusDepth);
    applyCenter(ui);  // octave from the soul
  }
  g_weather.start = nowSec();
  backgroundApply();
  printStatus(ui);
  double lastStatusAt = nowSec();
  if (soulLoaded) {
    printf("\n  (pamietam cie: tlo %d nut, %d wspomnien z poprzedniej"
           " sesji)\n", g_bgCount, g_garden.count());
    if (g_garden.count() > 0) {
      // it greets you with one of your own notes from last time
      schedule(2.5, [&ui] {
        if (g_playedThisSession || g_garden.count() == 0) return;
        g_engine.noteOnWithPreset(1198, g_garden.freshestCents(), 0.3f,
                                  ui.preset, 1.0f);
        schedule(2.2, [] { g_engine.noteOff(1198); });
      });
    }
  }

  while (g_running) {
    const int b = readByte(0);

    if (b >= 0) {
      const char c = (char)b;
      KeyPos kp;
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
      } else if (c == '<' || c == '>') {
        gardenShake(ui, c == '>' ? 1.0f : -1.0f);
      } else if (c == 'X') {
        fumble();
      } else if (c == '\t') {
        ui.scale = (ui.scale + 1) % (int)ScaleId::Count;
        allOff();  // clean retuning
        backgroundApply();
      } else if (c == '`') {
        ui.preset = (ui.preset + 1) % kNumTimbrePresets;
        g_engine.setParam(Param::TimbrePreset, (float)ui.preset);
        g_chorus.setDepth(timbrePreset(ui.preset).chorusDepth);
      } else if (c == 0x7F || c == 0x08) {  // backspace
        ui.octave = (ui.octave + 1) % 4;
        applyCenter(ui);
      } else if (c == 'L') {
        ui.latch = !ui.latch;
      } else if (c == 'B') {
        // A/B the psychoacoustic bass (virtual pitch below ~250 Hz)
        static bool bassOn = true;
        bassOn = !bassOn;
        g_engine.setParam(Param::BassVoicing, bassOn ? 1.0f : 0.0f);
        printf("\n  >> bas psychoakustyczny: %s\n", bassOn ? "ON" : "off");
      } else if (c == 'H') {
        g_harmony = !g_harmony;
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
        // The looper only accepts a capture into an empty loop or one of
        // the SAME length (mirrors applyCommands) — wiping the tape on a
        // rejected capture would lose the memory and produce no floor.
        const Looper::State ls = g_looper.state();
        const bool accepted =
            ls == Looper::State::Empty ||
            ((ls == Looper::State::Play || ls == Looper::State::Stopped ||
              ls == Looper::State::Overdub) &&
             g_looper.lengthFrames() == g_echoFrames);
        if (accepted) {
          g_looper.requestCapture(g_echoStorage, g_echoFrames);
          schedule(0.08, [] { g_echo.clearTape(); });
          printf("\n  >> FREEZE — ostatnie %.0f s pamieci staje sie podloga\n",
                 (double)kEchoSec);
        } else {
          printf("\n  >> FREEZE niemozliwy: petla ma inna dlugosc"
                 " (SHIFT+C czysci)\n");
        }
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
        gardenSilenceGhosts();
        gardenSilenceShake();
        g_sim.inFlight = false;
        g_sim.centerCents = 0.0f;
        g_sim.rateBase = 1.0f;
        setLoopRate(1.0f);
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
        setLoopRate(1.0f);
        applyCenter(ui);
        backgroundApply();
      } else if (keyToGrid(c, kp)) {
        // presence = real musical keys only: arrows, ESC sequences and
        // control chords must not count as playing (every raw byte used to
        // silence the ghosts, so waving the filter shooed them away)
        g_ghost.lastInput = nowSec();
        gardenSilenceGhosts();
        gardenSilenceShake();
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
        // harmonic shadow: the key one row above plays along quietly —
        // with rows stacked in thirds (PENTA/MAJOR) that is a real harmony
        const int hRow = kp.row < 3 ? kp.row + 1 : kp.row - 1;
        const float hCents = gridToCents((ScaleId)ui.scale, kp.col, hRow);
        if (ui.latch) {
          if (st.on) {
            g_engine.noteOff(id);
            g_engine.noteOff(id + 64);
            st = NoteState{};
          } else {
            g_engine.noteOn(id, cents, vel);
            if (g_harmony) g_engine.noteOn(id + 64, hCents, vel * 0.45f);
            gardenPush(cents);
            pulseOnOnset();
            st.on = true;
            st.latched = true;
          }
        } else {
          // press/auto-repeat plays and extends; fades out 0.6 s later
          g_engine.noteOn(id, cents, vel);
          if (g_harmony) g_engine.noteOn(id + 64, hCents, vel * 0.45f);
          if (!st.on) {
            gardenPush(cents);  // remember presses, not auto-repeats
            pulseOnOnset();
          }
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
    drainGardenOffs();
    weatherTick(ui);
    shakePhraseTick(ui, nowSec());
    gardenTick(ui);
    pulseTick();
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
        g_engine.noteOff(id + 64);  // the harmonic shadow follows
        g_notes[id].on = false;
      }
    }

    sleepMs(8);  // ~125 Hz tick: smooth flight, negligible input latency
  }

  gardenSilenceGhosts();
  gardenSilenceShake();
  soulSave(ui);
  if (g_garden.count() > 0) {
    // goodnight: it hums your last remembered note as it falls asleep
    g_engine.noteOnWithPreset(1199, g_garden.freshestCents(), 0.25f,
                              ui.preset, 0.12f);
    sleepMs(500);
    g_engine.noteOff(1199);
    sleepMs(300);
  }
  allOff();
  sleepMs(150);  // let the release tail ring before closing the queue
  AudioQueueStop(queue, true);
  AudioQueueDispose(queue, true);
  tcsetattr(STDIN_FILENO, TCSANOW, &orig);
  printf("\nbye!\n");
  return 0;
}
