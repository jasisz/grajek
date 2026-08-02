#include "goodnight.h"

#include <Arduino.h>

#include "ambient.h"
#include "modes/face_down_gesture.h"

namespace {

FaceDownGesture s_gesture;

}  // namespace

namespace goodnight {

void sample(uint32_t nowMs, float accelZ) {
  switch (s_gesture.update(nowMs, accelZ, ambient::lullabyActive())) {
    case FaceDownGesture::Event::Sleep:
      if (ambient::lullabyStart())
        Serial.println("grajek: dobranoc — kolysanka");
      break;
    case FaceDownGesture::Event::Wake:
      if (ambient::lullabyActive()) {
        Serial.println("grajek: pobudka");
        ambient::lullabyAbort();
      }
      break;
    case FaceDownGesture::Event::None:
      break;
  }
}

void wakeFromKey() {
  s_gesture.restartDwell();
  if (ambient::lullabyActive()) {
    Serial.println("grajek: pobudka");
    ambient::lullabyAbort();
  }
}

}  // namespace goodnight
