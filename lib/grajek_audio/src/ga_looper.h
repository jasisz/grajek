// Portable layered looper core.
//
// Storage is caller-provided int16 buffers (mix / layer / undo), so the same
// code runs on the host (minutes of loop in heap RAM) and on the ESP32
// (a few seconds in internal RAM, possibly at a lower sample rate). The
// looper records the input and ADDS loop playback into the output; it never
// passes the input through — input monitoring is the caller's decision
// (on the host the dry synth is already in the buffer; on the device a mic
// pass-through could feed back into the speaker).
//
// Threading contract mirrors ga::Engine: control-thread calls only set
// atomic command bits; process() — the audio thread — applies them at block
// boundaries.
//
// Flow: toggleRecord() from Empty starts the first recording, the next
// toggleRecord() closes it and defines the loop length; from then on
// toggleRecord() toggles overdub (committed as a layer), undoLayer() removes
// the last committed layer (one level), togglePlay() stops/starts playback,
// clear() wipes everything.
#pragma once
#include <stdint.h>
#include <atomic>

namespace ga {

class Looper {
 public:
  enum class State : uint8_t { Empty, RecordFirst, Play, Overdub, Stopped };

  // mix and layer must each hold maxFrames samples. undoStack holds
  // undoDepth slots of maxFrames each (a ring of pre-commit snapshots =
  // multi-level undo); depth 0 / nullptr disables undo entirely.
  void init(int16_t* mix, int16_t* layer, int16_t* undoStack, int undoDepth,
            uint32_t maxFrames);
  // Backward-compatible single-buffer variant (one undo level).
  void init(int16_t* mix, int16_t* layer, int16_t* undo, uint32_t maxFrames) {
    init(mix, layer, undo, undo ? 1 : 0, maxFrames);
  }

  // --- control thread ---
  void toggleRecord() { cmds_.fetch_or(kCmdRecord, std::memory_order_release); }
  void togglePlay()   { cmds_.fetch_or(kCmdPlay,   std::memory_order_release); }
  // During an overdub: cancels the current take ("wait, not that").
  // Otherwise: removes the last committed layer (up to undoDepth levels).
  void undoLayer()    { cmds_.fetch_or(kCmdUndo,   std::memory_order_release); }
  void clear()        { cmds_.fetch_or(kCmdClear,  std::memory_order_release); }

  // Loop playback level (0..1.5). Recording always captures the input dry;
  // this only scales what the loop adds to the output — turn it down to
  // leave headroom for playing over the loop.
  void setPlaybackLevel(float v) {
    if (v < 0.0f) v = 0.0f;
    if (v > 1.5f) v = 1.5f;
    level_.store(v, std::memory_order_relaxed);
  }
  float playbackLevel() const { return level_.load(std::memory_order_relaxed); }

  // Varispeed: tape-style playback rate (pitch and tempo together), smoothed
  // in the audio thread. 1.0 = as recorded. Used by the toss gesture — the
  // whole loop lifts in flight and lands on a new tonal center — and by the
  // fumble crash (rate dive). Overdub writing pauses while rate is away from
  // 1.0 (positions would smear across the layer buffer).
  void setRate(float v) {
    if (v < 0.1f) v = 0.1f;
    if (v > 4.0f) v = 4.0f;
    rateTarget_.store(v, std::memory_order_relaxed);
  }
  float rate() const { return rateTarget_.load(std::memory_order_relaxed); }

  // --- audio thread: records `in`, ADDS loop playback into `out` ---
  // `in` and `out` may alias (the input is read before the output is written).
  void process(const float* in, float* out, int n);

  // --- UI reads (any thread, relaxed) ---
  State state() const { return state_.load(std::memory_order_relaxed); }
  uint32_t lengthFrames() const { return length_.load(std::memory_order_relaxed); }
  uint32_t positionFrames() const { return pos_.load(std::memory_order_relaxed); }
  int layerCount() const { return layers_.load(std::memory_order_relaxed); }
  int undoCount() const { return undoCount_.load(std::memory_order_relaxed); }
  bool undoAvailable() const { return undoCount() > 0; }

 private:
  static constexpr uint32_t kCmdRecord = 1, kCmdPlay = 2, kCmdUndo = 4,
                            kCmdClear = 8;

  static int16_t sat(float v) {
    if (v > 32767.0f) return 32767;
    if (v < -32768.0f) return -32768;
    return (int16_t)v;
  }
  static int16_t satAdd(int16_t a, int16_t b) {
    const int32_t s = (int32_t)a + (int32_t)b;
    if (s > 32767) return 32767;
    if (s < -32768) return -32768;
    return (int16_t)s;
  }

  void applyCommands();
  void commitLayer();

  int16_t* mix_ = nullptr;
  int16_t* layer_ = nullptr;
  int16_t* undo_ = nullptr;  // base of the snapshot ring (undoDepth_ slots)
  int undoDepth_ = 0;
  int undoHead_ = 0;  // audio-thread only: next slot to write
  uint32_t max_ = 0;

  std::atomic<uint32_t> cmds_{0};
  std::atomic<float> level_{1.0f};
  std::atomic<float> rateTarget_{1.0f};
  float rate_ = 1.0f;   // audio-thread only, smoothed toward rateTarget_
  double posF_ = 0.0;   // audio-thread only, fractional playhead
  std::atomic<State> state_{State::Empty};
  std::atomic<uint32_t> length_{0};
  std::atomic<uint32_t> pos_{0};
  std::atomic<int> layers_{0};
  std::atomic<int> undoCount_{0};
};

}  // namespace ga
