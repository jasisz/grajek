#include "goodnight.h"

#include <Arduino.h>

#include "ambient.h"
#include "modes/face_down_gesture.h"
#include "modes/idle_sleep.h"

namespace {

FaceDownGesture s_gesture;
IdleSleep s_idle;

}  // namespace

namespace goodnight {

void sample(uint32_t nowMs, float accelZ) {
  const bool sleeping = ambient::lullabyActive();
  switch (s_gesture.update(nowMs, accelZ, sleeping)) {
    case FaceDownGesture::Event::Sleep:
      if (ambient::lullabyStart())
        Serial.println("grajek: dobranoc — kolysanka");
      break;
    case FaceDownGesture::Event::Wake:
      // Being lifted out of face-down is a person arriving: wake, and give
      // the inactivity clock a fresh start rather than the stale reading
      // from before the box went to sleep.
      s_idle.noteActivity(nowMs);
      if (sleeping) {
        Serial.println("grajek: pobudka");
        ambient::lullabyAbort();
      }
      break;
    case FaceDownGesture::Event::None:
      break;
  }

  // The other road to the same ritual: nobody has played for a long time.
  if (s_idle.update(nowMs, ambient::lullabyActive()))
    if (ambient::lullabyStart())
      Serial.println("grajek: cisza od dawna — kolysanka");
}

void noteActivity() { s_idle.noteActivity(millis()); }

void wakeFromKey() {
  s_gesture.restartDwell();
  s_idle.noteActivity(millis());
  if (ambient::lullabyActive()) {
    Serial.println("grajek: pobudka");
    ambient::lullabyAbort();
  }
}

}  // namespace goodnight
