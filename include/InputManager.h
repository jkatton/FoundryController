#pragma once
#include <Arduino.h>
#include "Types.h"

// Handles the quadrature rotary encoder (interrupt driven, still native
// GPIO - see config.h for why) plus everything on the RKJXT1F42001 +
// PCF8575 expander: the encoder's center push ("Button A"), the 4-direction
// tilt switch, and the standalone Button B. The expander pins are simple
// switch-to-GND closures read over I2C instead of digitalRead(), but they
// go through the exact same debounced short/long/double-press state
// machine as before.
//
// Call InputManager::begin() once from setup(), and poll every
// button/direction state machine from loop() via pollButtonA()/
// pollButtonB()/pollJoyUp()/pollJoyDown()/pollJoyLeft()/pollJoyRight()
// every iteration (cheap, non-blocking - all of them share one cached I2C
// read per loop iteration; see expanderState() in the .cpp).
namespace InputManager {

  void begin();

  // Runtime toggle for verbose Serial debug output ("Serial Debug Output"
  // on the settings screen) - defaults to INPUT_DEBUG from config.h. Gates
  // the encoder/button/joystick debug prints below, and the error-message
  // echo in Display::showError().
  void setDebugEnabled(bool enabled);
  bool debugEnabled();

  // Returns accumulated encoder steps (positive = clockwise) since the last
  // call, then resets the internal counter to zero. Safe to call every loop.
  enum class EncoderProfile : uint8_t { NORMAL, IP_ENTRY };

int32_t readEncoderDelta(EncoderProfile profile = EncoderProfile::NORMAL);

  // Must be called every loop() iteration for each button/direction;
  // returns SHORT_PRESS, LONG_PRESS, or DOUBLE_PRESS exactly once per
  // physical press, or NONE.
  ButtonEvent pollButtonA(); // RKJXT1F42001 center push (PCF8575 P0)
  ButtonEvent pollButtonB(); // standalone second button (PCF8575 P5)

  // RKJXT1F42001 4-direction tilt switch (PCF8575 P1-P4). Same debounced
  // short/long/double-press machine as the buttons above - e.g. a
  // long-press-left is a perfectly valid event if a screen wants one.
  ButtonEvent pollJoyUp();
  ButtonEvent pollJoyDown();
  ButtonEvent pollJoyLeft();
  ButtonEvent pollJoyRight();

}