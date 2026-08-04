// SPSC event queue (control thread -> audio thread), lock-free.
#pragma once
#include <stdint.h>
#include <atomic>

namespace ga {

enum class Param : uint8_t {
  MasterGain,      // 0..1
  FilterCutoffHz,  // 40..14000
  FilterRes,       // 0..1
  BendCents,       // global pitch offset (IMU), in cents
  GlideSec,        // portamento time for new notes
  BaseHz,          // grid base frequency (cents=0)
  EnvAttack,       // seconds — affects notes played after the change
  EnvDecay,
  EnvSustain,      // 0..1
  EnvRelease,
  TimbrePreset,    // timbre preset index (int carried in float)
  BassVoicing,     // >0.5 = psychoacoustic bass for tiny speakers (default on)
};

struct Event {
  enum Type : uint8_t { None, NoteOn, NoteOff, AllOff, SetParam };
  Type type = None;
  Param param = Param::MasterGain;
  // -1 uses the player's current timbre. Ambient voices can name a preset on
  // their own event without temporarily mutating the global colour/filter.
  int8_t preset = -1;
  bool persistent = false;  // NoteOn: steal only as a last resort
  int32_t id = 0;   // note identifier (e.g. key index)
  float a = 0.0f;   // NoteOn: cents; SetParam: value
  float b = 0.0f;   // NoteOn: velocity 0..1
  // NoteOn: explicit glide start, or attack seconds when attackOverride=true.
  float c = 0.0f;
  bool glideFrom = false;   // NoteOn: start at c, then slew to a
  bool attackOverride = false;
  // NoteOn with glideFrom: how long the landing takes. Milliseconds fit the
  // struct's existing padding, so the queue message does not grow.
  uint8_t glideMs = 45;
};
static_assert(sizeof(Event) <= 24, "EventQueue control messages grew too large");

// Single producer (UI/input), single consumer (audio). N = power of two.
template <int N>
class EventQueue {
  static_assert((N & (N - 1)) == 0, "N must be a power of two");
 public:
  bool push(const Event& e) {
    const uint32_t h = head_.load(std::memory_order_relaxed);
    const uint32_t t = tail_.load(std::memory_order_acquire);
    if (h - t >= (uint32_t)N) return false;  // full — event is dropped
    buf_[h & (N - 1)] = e;
    head_.store(h + 1, std::memory_order_release);
    return true;
  }
  bool pop(Event& e) {
    const uint32_t t = tail_.load(std::memory_order_relaxed);
    const uint32_t h = head_.load(std::memory_order_acquire);
    if (t == h) return false;
    e = buf_[t & (N - 1)];
    tail_.store(t + 1, std::memory_order_release);
    return true;
  }
 private:
  Event buf_[N];
  std::atomic<uint32_t> head_{0};
  std::atomic<uint32_t> tail_{0};
};

}  // namespace ga
