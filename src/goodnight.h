#pragma once

#include <stdint.h>

// The goodnight ritual is a behaviour of the whole box, not a play mode.
// Both the instrument and settings screen feed it IMU samples so changing
// screens cannot interrupt a face-down dwell or leave the box asleep.
namespace goodnight {

void sample(uint32_t nowMs, float accelZ);
void wakeFromKey();

}  // namespace goodnight
