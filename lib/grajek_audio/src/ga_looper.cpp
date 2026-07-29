#include "ga_looper.h"

#include <string.h>

namespace ga {

void Looper::init(int16_t* mix, int16_t* layer, int16_t* undo,
                  uint32_t maxFrames) {
  mix_ = mix;
  layer_ = layer;
  undo_ = undo;
  max_ = maxFrames;
  cmds_.store(0);
  state_.store(State::Empty);
  length_.store(0);
  pos_.store(0);
  layers_.store(0);
  undoOk_.store(false);
  memset(mix_, 0, maxFrames * sizeof(int16_t));
  memset(layer_, 0, maxFrames * sizeof(int16_t));
  if (undo_) memset(undo_, 0, maxFrames * sizeof(int16_t));
}

// Commit is up to three passes over the loop (undo snapshot, add, zero).
// Worst case on the host (60 s @ 48 kHz) that is a few ms inside the audio
// callback — absorbed by the output queue cushion; on the device loops are
// short enough for this to be trivial.
void Looper::commitLayer() {
  const uint32_t len = length_.load(std::memory_order_relaxed);
  if (undo_) {
    memcpy(undo_, mix_, len * sizeof(int16_t));
    undoOk_.store(true, std::memory_order_relaxed);
  }
  for (uint32_t i = 0; i < len; ++i) mix_[i] = satAdd(mix_[i], layer_[i]);
  memset(layer_, 0, len * sizeof(int16_t));
  layers_.fetch_add(1, std::memory_order_relaxed);
}

void Looper::applyCommands() {
  const uint32_t c = cmds_.exchange(0, std::memory_order_acquire);
  if (!c) return;
  State s = state_.load(std::memory_order_relaxed);

  if (c & kCmdClear) {
    // layer_ may hold an abandoned overdub — wipe it; mix_ is always
    // overwritten by the next first recording, so it can stay dirty.
    memset(layer_, 0, length_.load(std::memory_order_relaxed) * sizeof(int16_t));
    state_.store(State::Empty);
    length_.store(0);
    pos_.store(0);
    layers_.store(0);
    undoOk_.store(false);
    return;
  }

  if (c & kCmdRecord) {
    switch (s) {
      case State::Empty:
        s = State::RecordFirst;
        length_.store(0);
        pos_.store(0);
        break;
      case State::RecordFirst:
        if (length_.load(std::memory_order_relaxed) > 0) {
          s = State::Play;
          pos_.store(0);
          layers_.store(1);
          undoOk_.store(false);
        } else {
          s = State::Empty;  // zero-length take: nothing to keep
        }
        break;
      case State::Play:
        s = State::Overdub;
        break;
      case State::Stopped:
        s = State::Overdub;  // restart playback and record on top
        pos_.store(0);
        break;
      case State::Overdub:
        commitLayer();
        s = State::Play;
        break;
    }
  }

  if (c & kCmdPlay) {
    switch (s) {
      case State::Play:
        s = State::Stopped;
        pos_.store(0);
        break;
      case State::Overdub:
        commitLayer();
        s = State::Stopped;
        pos_.store(0);
        break;
      case State::Stopped:
        s = State::Play;
        break;
      default:
        break;  // Empty / RecordFirst: nothing to play
    }
  }

  if (c & kCmdUndo) {
    if ((s == State::Play || s == State::Stopped) && undo_ &&
        undoOk_.load(std::memory_order_relaxed)) {
      memcpy(mix_, undo_,
             length_.load(std::memory_order_relaxed) * sizeof(int16_t));
      layers_.fetch_sub(1, std::memory_order_relaxed);
      undoOk_.store(false, std::memory_order_relaxed);
    }
  }

  state_.store(s, std::memory_order_relaxed);
}

void Looper::process(const float* in, float* out, int n) {
  applyCommands();
  const State s = state_.load(std::memory_order_relaxed);

  switch (s) {
    case State::Empty:
    case State::Stopped:
      return;

    case State::RecordFirst: {
      uint32_t w = length_.load(std::memory_order_relaxed);
      for (int i = 0; i < n; ++i) {
        if (w >= max_) {  // capacity reached: close the loop automatically
          length_.store(w);
          layers_.store(1);
          pos_.store(0);
          undoOk_.store(false);
          state_.store(State::Play);
          return;
        }
        mix_[w++] = sat(in[i] * 32767.0f);
      }
      length_.store(w);
      return;
    }

    case State::Play:
    case State::Overdub: {
      const uint32_t len = length_.load(std::memory_order_relaxed);
      if (len == 0) return;
      uint32_t p = pos_.load(std::memory_order_relaxed);
      const float k =
          (1.0f / 32768.0f) * level_.load(std::memory_order_relaxed);
      for (int i = 0; i < n; ++i) {
        float playback = (float)mix_[p];
        if (s == State::Overdub) {
          // Read the layer BEFORE adding this sample: material from earlier
          // wraps of the session is audible, but the live input is not
          // doubled (it is already heard dry via the caller's path).
          const int16_t prev = layer_[p];
          layer_[p] = satAdd(prev, sat(in[i] * 32767.0f));
          playback += (float)prev;
        }
        out[i] += playback * k;
        if (++p >= len) p = 0;
      }
      pos_.store(p);
      return;
    }
  }
}

}  // namespace ga
