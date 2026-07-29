#include "ga_looper.h"

#include <math.h>
#include <string.h>

namespace ga {

void Looper::init(int16_t* mix, int16_t* layer, int16_t* undoStack,
                  int undoDepth, uint32_t maxFrames) {
  mix_ = mix;
  layer_ = layer;
  undo_ = undoStack;
  undoDepth_ = (undoStack && undoDepth > 0) ? undoDepth : 0;
  undoHead_ = 0;
  max_ = maxFrames;
  cmds_.store(0);
  state_.store(State::Empty);
  length_.store(0);
  pos_.store(0);
  posF_ = 0.0;
  rate_ = 1.0f;
  rateTarget_.store(1.0f);
  layers_.store(0);
  undoCount_.store(0);
  memset(mix_, 0, maxFrames * sizeof(int16_t));
  memset(layer_, 0, maxFrames * sizeof(int16_t));
  if (undo_)
    memset(undo_, 0, (size_t)undoDepth_ * maxFrames * sizeof(int16_t));
}

// Commit is a few passes over the loop (snapshot, add, zero). Worst case on
// the host (60 s @ 48 kHz) that is a few ms inside the audio callback —
// absorbed by the output queue cushion; device loops are short enough for
// this to be trivial.
void Looper::commitLayer() {
  const uint32_t len = length_.load(std::memory_order_relaxed);
  if (undoDepth_ > 0) {
    memcpy(undo_ + (size_t)undoHead_ * max_, mix_, len * sizeof(int16_t));
    undoHead_ = (undoHead_ + 1) % undoDepth_;
    const int c = undoCount_.load(std::memory_order_relaxed);
    if (c < undoDepth_) undoCount_.store(c + 1, std::memory_order_relaxed);
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
    posF_ = 0.0;
    layers_.store(0);
    undoCount_.store(0);
    undoHead_ = 0;
    return;
  }

  if (c & kCmdRecord) {
    switch (s) {
      case State::Empty:
        s = State::RecordFirst;
        length_.store(0);
        pos_.store(0);
        posF_ = 0.0;
        break;
      case State::RecordFirst:
        if (length_.load(std::memory_order_relaxed) > 0) {
          s = State::Play;
          pos_.store(0);
          posF_ = 0.0;
          layers_.store(1);
          undoCount_.store(0);
          undoHead_ = 0;
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
        posF_ = 0.0;
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
        posF_ = 0.0;
        break;
      case State::Overdub:
        commitLayer();
        s = State::Stopped;
        pos_.store(0);
        posF_ = 0.0;
        break;
      case State::Stopped:
        s = State::Play;
        break;
      default:
        break;  // Empty / RecordFirst: nothing to play
    }
  }

  if (c & kCmdUndo) {
    if (s == State::Overdub) {
      // cancel the current take — discard the uncommitted layer
      memset(layer_, 0,
             length_.load(std::memory_order_relaxed) * sizeof(int16_t));
      s = State::Play;
    } else if ((s == State::Play || s == State::Stopped) && undoDepth_ > 0) {
      const int cnt = undoCount_.load(std::memory_order_relaxed);
      if (cnt > 0) {
        undoHead_ = (undoHead_ - 1 + undoDepth_) % undoDepth_;
        memcpy(mix_, undo_ + (size_t)undoHead_ * max_,
               length_.load(std::memory_order_relaxed) * sizeof(int16_t));
        layers_.fetch_sub(1, std::memory_order_relaxed);
        undoCount_.store(cnt - 1, std::memory_order_relaxed);
      }
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
          posF_ = 0.0;
          undoCount_.store(0);
          undoHead_ = 0;
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
      // varispeed: smooth the rate per block, read with linear interpolation
      rate_ += 0.03f * (rateTarget_.load(std::memory_order_relaxed) - rate_);
      double p = posF_;
      if (p >= (double)len) p = 0.0;
      const float k =
          (1.0f / 32768.0f) * level_.load(std::memory_order_relaxed);
      for (int i = 0; i < n; ++i) {
        const uint32_t i0 = (uint32_t)p;
        uint32_t i1 = i0 + 1;
        if (i1 >= len) i1 = 0;
        const float frac = (float)(p - (double)i0);
        float playback =
            (float)mix_[i0] * (1.0f - frac) + (float)mix_[i1] * frac;
        if (s == State::Overdub) {
          // Read the layer BEFORE writing this sample: earlier wraps of the
          // session stay audible, the live input is not doubled (it is heard
          // dry via the caller's path).
          const float prevL =
              (float)layer_[i0] * (1.0f - frac) + (float)layer_[i1] * frac;
          // Tape-head write at any speed: splat the sample across the two
          // neighbouring positions, weighted like the read interpolation.
          // Off-speed takes replay slower/lower once the tape returns to 1x —
          // that is intended (tape-loop behaviour), not a bug.
          const float v = in[i] * 32767.0f;
          layer_[i0] = satAdd(layer_[i0], sat(v * (1.0f - frac)));
          layer_[i1] = satAdd(layer_[i1], sat(v * frac));
          playback += prevL;
        }
        out[i] += playback * k;
        p += (double)rate_;
        while (p >= (double)len) p -= (double)len;
      }
      posF_ = p;
      pos_.store((uint32_t)p);
      return;
    }
  }
}

}  // namespace ga
