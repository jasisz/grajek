#pragma once

#include <stdint.h>

// The goodnight ritual is a behaviour of the whole box, not a play mode.
// Both the instrument and settings screen feed it IMU samples so changing
// screens cannot interrupt a face-down dwell or leave the box asleep.
namespace goodnight {

// Feed every pass: drives the face-down dwell and the inactivity timer.
void sample(uint32_t nowMs, float accelZ);

// A real key or the side button: wakes the box and restarts both clocks.
void wakeFromKey();

// A deliberate gesture that is not a key — a swing that plays a chime. It
// keeps the box awake but never wakes a sleeping one, because a sleeping box
// lying face down is allowed to be jostled without ending its lullaby.
void noteActivity();

}  // namespace goodnight
