#pragma once
#include <Arduino.h>
#include "Types.h"

// Handles the quadrature rotary encoder (interrupt driven) and debounced
// short/long press detection for both buttons. Call InputManager::begin()
// once from setup(), and poll the button state machines from loop() via
// pollButtonA()/pollButtonB() every iteration (cheap, non-blocking).
namespace InputManager {

  void begin();

  // Runtime toggle for verbose Serial debug output ("Serial Debug Output"
  // on the settings screen) - defaults to INPUT_DEBUG from config.h. Gates
  // the encoder/button debug prints below, and the error-message echo in
  // Display::showError().
  void setDebugEnabled(bool enabled);
  bool debugEnabled();

  // Returns accumulated encoder steps (positive = clockwise) since the last
  // call, then resets the internal counter to zero. Safe to call every loop.
  enum class EncoderProfile : uint8_t { NORMAL, IP_ENTRY };

int32_t readEncoderDelta(EncoderProfile profile = EncoderProfile::NORMAL);

  // Must be called every loop() iteration for each button; returns
  // SHORT_PRESS or LONG_PRESS exactly once per physical press, or NONE.
  ButtonEvent pollButtonA(); // encoder shaft button
  ButtonEvent pollButtonB(); // second standalone button

}
