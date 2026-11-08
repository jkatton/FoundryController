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

  // Runtime toggle for verbose Serial debug output ("Debug Mode"
  // on the settings screen) - defaults to INPUT_DEBUG from config.h. Gates
  // all Serial output (encoder/button/joystick prints, init warnings) and,
  // via main.cpp's goToError(), whether non-fatal error screens are shown.
  void setDebugEnabled(bool enabled);
  bool debugEnabled();

  // Runtime toggle for "Swap A/B" on the settings screen - defaults to
  // false (no swap). When true, pollButtonA()/pollButtonB() below read
  // from each other's physical PCF8575 pin, so Button A's physical switch
  // starts reporting the events Button B used to (and vice versa),
  // including the quadrature-quirk masking that's specific to the
  // encoder's own center-push pin (see anyDirectionRawPressed() in the
  // .cpp) - that masking stays tied to the physical pin it's wired to,
  // not to whichever logical role currently reads it. Display.cpp's
  // labelA()/labelB() call buttonsSwapped() too, so on-screen hint text
  // (e.g. "<A>:next") always names whichever physical button currently
  // performs that role.
  void setButtonsSwapped(bool swapped);
  bool buttonsSwapped();

  // Overrides how long (after debounce) a joystick direction must stay
  // pressed before pollJoy*() reports it - defaults to DIR_MIN_HOLD_MS.
  // main.cpp raises it to DIR_START_HOLD_MS while no move is in progress on
  // the detail screen, so a brush against the stick can't open move mode.
  void setDirectionMinHoldMs(uint16_t ms);

  // Returns accumulated encoder steps (positive = clockwise) since the last
  // call, then resets the internal counter to zero. Safe to call every loop.
  enum class EncoderProfile : uint8_t { NORMAL, IP_ENTRY };

int32_t readEncoderDelta(EncoderProfile profile = EncoderProfile::NORMAL);

  // Must be called every loop() iteration for each button/direction;
  // returns SHORT_PRESS, LONG_PRESS, or DOUBLE_PRESS exactly once per
  // physical press, or NONE.
  ButtonEvent pollButtonA(); // RKJXT1F42001 center push (PCF8575 P0)
  ButtonEvent pollButtonB(); // standalone second button (PCF8575 P5)

  // Instantaneous raw state of the two physical buttons, for the boot-time
  // "both buttons held" check (see main.cpp's bootChordHeld()). These do NOT
  // go through the debounce/short/long/double-press machine and don't care
  // about the Swap A/B setting - they look at the physical pins. Both
  // ignore the tilt switch's center-contact quirk (a closed direction also
  // closes Button A's pin), so tilting the stick never counts as a push.
  //   bothButtonsHeld(): both the center push and Button B are closed.
  //   anyButtonHeld():   either one is closed.
  bool bothButtonsHeld();
  bool anyButtonHeld();

  // RKJXT1F42001 4-direction tilt switch (PCF8575 P1-P4). Unlike the
  // buttons above, directions are always instant, single-shot presses -
  // there's no long-press or double-press meaning for a direction, so
  // these skip that debounced classification machine entirely (no
  // pairing wait before reporting a press, no long-press timer). Only
  // ever returns SHORT_PRESS (the instant a press debounces) or NONE.
  ButtonEvent pollJoyUp();
  ButtonEvent pollJoyDown();
  ButtonEvent pollJoyLeft();
  ButtonEvent pollJoyRight();

  // Press-and-hold auto-repeat, for continuous movement while a direction
  // is held rather than one nudge per physical press. Returns true once
  // every MOVE_REPEAT_INTERVAL_MS (config.h) after a direction has been
  // held past MOVE_REPEAT_DELAY_MS; false otherwise, including on the
  // initial press itself (that first nudge is still the caller's job via
  // the SHORT_PRESS from pollJoyUp() etc. above). Independent of the
  // short/long/double-press machine - call both every tick for a given
  // direction; this neither consumes nor is affected by the other's
  // result.
  bool pollJoyUpRepeat();
  bool pollJoyDownRepeat();
  bool pollJoyLeftRepeat();
  bool pollJoyRightRepeat();

}