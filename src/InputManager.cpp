#include "InputManager.h"
#include "config.h"
#include <Wire.h>

namespace InputManager
{

  // Runtime-editable (see setDebugEnabled()/debugEnabled(), and the
  // "Debug Mode" entry on the settings screen) - defaults to
  // INPUT_DEBUG from config.h. Replaces what used to be a compile-time
  // #if INPUT_DEBUG guard around every Serial.printf() below.
  static bool s_debugEnabled = false;

  void setDebugEnabled(bool enabled) { s_debugEnabled = enabled; }
  bool debugEnabled() { return s_debugEnabled; }

  // Runtime-editable (see setButtonsSwapped()/buttonsSwapped(), and the
  // "Swap A/B" entry on the settings screen) - defaults to false (no
  // swap), same pattern as s_debugEnabled above. Read by pollButtonA()/
  // pollButtonB() further down.
  static bool s_buttonsSwapped = false;

  void setButtonsSwapped(bool swapped) { s_buttonsSwapped = swapped; }
  bool buttonsSwapped() { return s_buttonsSwapped; }

  // See setDirectionMinHoldMs() in InputManager.h.
  static uint16_t s_dirMinHoldMs = DIR_MIN_HOLD_MS;
  void setDirectionMinHoldMs(uint16_t ms) { s_dirMinHoldMs = ms; }

  // ---------------------------------------------------------------------
  // Rotary encoder - interrupt driven quadrature decode
  // ---------------------------------------------------------------------
  // Unchanged by the RKJXT1F42001 switch: CLK/DT stay on native GPIO (see
  // config.h for why quadrature decoding can't move to the I2C expander).
  static volatile int32_t s_rawCounter = 0;
  static int32_t s_lastProcessed = 0;
  static volatile uint8_t s_lastAB = 0;
  static unsigned long s_lastStepMs = 0; // timestamp of last nonzero step, for acceleration

  // millis() deadline until which joystick directions are ignored because
  // the encoder was just turned (see DIR_ENCODER_LOCKOUT_MS in config.h).
  // Written from the ISR on every quadrature edge; compared with signed
  // subtraction so millis() rollover is harmless.
  static volatile unsigned long s_encoderGuardUntilMs = 0;

  static const int8_t QUAD_TABLE[16] = {
      0, +1, -1, 0,
      -1, 0, 0, +1,
      +1, 0, 0, -1,
      0, -1, +1, 0};

  // Acceleration tiers: if the time-per-detent since the last read is under
  // maxMsPerDetent, apply the paired multiplier. Table must stay ordered
  // fastest-first; the last entry (UINT32_MAX) is the "no acceleration" floor.
  struct AccelTier
  {
    unsigned long maxMsPerDetent;
    int32_t multiplier;
  };

  // Existing table, used everywhere except IP entry.
  static const AccelTier ACCEL_TABLE_NORMAL[] = {
      {15, 5},
      {30, 2},
      {0xFFFFFFFFUL, 1},
  };

  // Gentler table for the IP entry screen - octet fields only span 0-255,
  // so the 20x/10x tiers above make it too easy to blow past the value you
  // wanted. Port field still gets some benefit from the 2x/4x tiers.
  static const AccelTier ACCEL_TABLE_IP_ENTRY[] = {
      {15, 20},
      {30, 10},
      {60, 4},
      {120, 2},
      {0xFFFFFFFFUL, 1},
  };

  void IRAM_ATTR encoderISR()
  {
    uint8_t a = digitalRead(PIN_ENCODER_CLK);
    uint8_t b = digitalRead(PIN_ENCODER_DT);
    uint8_t curAB = (a << 1) | b;
    uint8_t index = (s_lastAB << 2) | curAB;
    s_rawCounter += QUAD_TABLE[index & 0x0F];
    s_lastAB = curAB;
    s_encoderGuardUntilMs = millis() + DIR_ENCODER_LOCKOUT_MS;
  }

  int32_t readEncoderDelta(EncoderProfile profile)
  {
    noInterrupts();
    int32_t current = s_rawCounter;
    interrupts();

    int32_t diff = current - s_lastProcessed;
    // New encoder outputs 2 raw quadrature transitions per detent (was 4
    // on the old part) - divisor/multiplier below updated to match, or
    // every other detent would get silently swallowed.
    int32_t steps = diff / 2;
    s_lastProcessed += steps * 2;

    if (steps == 0)
    {
      return 0;
    }

    unsigned long now = millis();
    int32_t magnitude = (steps < 0) ? -steps : steps;

    unsigned long msSinceLastStep = now - s_lastStepMs;
    unsigned long msPerDetent = msSinceLastStep / (unsigned long)magnitude;

    const AccelTier *table = (profile == EncoderProfile::IP_ENTRY) ? ACCEL_TABLE_IP_ENTRY : ACCEL_TABLE_NORMAL;
    size_t tableLen = (profile == EncoderProfile::IP_ENTRY)
                          ? sizeof(ACCEL_TABLE_IP_ENTRY) / sizeof(ACCEL_TABLE_IP_ENTRY[0])
                          : sizeof(ACCEL_TABLE_NORMAL) / sizeof(ACCEL_TABLE_NORMAL[0]);

    int32_t multiplier = 1;
    for (size_t i = 0; i < tableLen; i++)
    {
      if (msPerDetent <= table[i].maxMsPerDetent)
      {
        multiplier = table[i].multiplier;
        break;
      }
    }

    s_lastStepMs = now;
    int32_t accelSteps = steps * multiplier;

    if (s_debugEnabled)
    {
      Serial.printf("[encoder] raw=%ld unconsumed=%ld steps=%ld msPerDetent=%lu mult=%ld accelSteps=%ld\n",
                    (long)current, (long)diff, (long)steps, msPerDetent, (long)multiplier, (long)accelSteps);
    }

    return accelSteps;
  }

  // ---------------------------------------------------------------------
  // PCF8575 I2C expander - encoder center push, joystick, Button B
  // ---------------------------------------------------------------------
  // Quasi-bidirectional: a pin reads as an input (with a weak internal
  // pull-up) as long as its output latch is left at 1, which is also the
  // power-on default - pcf8575Write(0xFFFF) in begin() makes that explicit
  // rather than relying on the power-on state alone. All six pins we use
  // (P0-P5) are simple switch-to-GND closures, so a read bit of 0 means
  // pressed, 1 means released - the same polarity as the old native
  // INPUT_PULLUP digitalRead() calls, so the debounce state machine below
  // is unchanged.
  static void pcf8575Write(uint16_t value)
  {
    Wire.beginTransmission(PCF8575_I2C_ADDR);
    Wire.write((uint8_t)(value & 0xFF));        // P0-P7
    Wire.write((uint8_t)((value >> 8) & 0xFF)); // P8-P15
    Wire.endTransmission();
  }

  static uint16_t pcf8575Read()
  {
    if (Wire.requestFrom((uint8_t)PCF8575_I2C_ADDR, (uint8_t)2) != 2)
    {
      // Bus error / expander not responding - fail safe as "everything
      // released" (all 1s) rather than spuriously reporting every button
      // as pressed.
      return 0xFFFF;
    }
    uint8_t lo = Wire.read(); // P0-P7
    uint8_t hi = Wire.read(); // P8-P15
    return (uint16_t)lo | ((uint16_t)hi << 8);
  }

  // Unlike pcf8575Read(), which deliberately can't be told apart from a
  // genuinely-idle expander (both cases return 0xFFFF - see its comment),
  // this checks the actual Wire.requestFrom() result directly, so callers
  // that specifically need to know "is anything out there responding" -
  // i.e. init-time retry logic in begin() - have a real answer instead of
  // an ambiguous one.
  static bool pcf8575Ping()
  {
    if (Wire.requestFrom((uint8_t)PCF8575_I2C_ADDR, (uint8_t)2) != 2)
    {
      return false;
    }
    Wire.read();
    Wire.read();
    return true;
  }

  // One I2C transaction covers all six pins we care about, so every
  // pollButton*()/pollJoy*() call in a given loop() iteration shares a
  // single read instead of hitting the bus once per pin. PIN_EXPANDER_INT
  // (see config.h) would let us skip reads entirely while idle, but at the
  // ~5ms loop cadence here a single 2-byte I2C transaction is already a
  // small fraction of the budget, so this simpler "once per millis()" cache
  // is used instead; /INT is left available for a future power-saving pass.
  static uint16_t s_expanderCache = 0xFFFF;
  static unsigned long s_expanderCacheMs = 0;
  static bool s_expanderCacheValid = false;

  // Whether the PCF8575 has ever successfully answered a ping. begin()'s
  // own retry loop gives it ~450ms at boot; if that's not enough (cold
  // boot, expander still settling), this used to stay false for the rest
  // of the session with nothing ever trying again - the only way to get
  // working buttons was a full reset (which is exactly what opening a
  // serial monitor forces via the USB adapter's DTR/RTS auto-reset, which
  // is why "buttons work once I open the monitor" was the symptom rather
  // than a coincidence). Retried quietly here instead, at a slow cadence,
  // for as long as it takes - no reset required. Deliberately only ever
  // goes through pcf8575Write()/pcf8575Ping() (the normal Wire API) - see
  // the big comment above begin() for why raw SDA/SCL pin manipulation is
  // specifically the wrong move on this board.
  static bool s_expanderReady = false;
  static unsigned long s_lastExpanderRetryMs = 0;
  static const unsigned long EXPANDER_RETRY_INTERVAL_MS = 250;

  // ---------------------------------------------------------------------
  // Joystick accidental-nudge guards
  // ---------------------------------------------------------------------
  // The tilt switch wobbles when the encoder is turned or pushed, which
  // would otherwise register as a direction press and start a move. Three
  // guards (a fourth, the minimum hold time, lives in pollDirection()):
  //   1. encoder activity lockout   - s_encoderGuardUntilMs, set by the ISR
  //   2. center-push lockout        - s_centerGuardUntilMs, below
  //   3. ambiguous multi-direction  - s_dirsAmbiguous, below
  // Evaluated once per fresh expander snapshot (see expanderState()).
  static unsigned long s_centerGuardUntilMs = 0;
  static unsigned long s_centerPureStartMs = 0; // 0 = center push not currently closed on its own
  static bool s_dirsAmbiguous = false;

  static inline bool guardActive(unsigned long untilMs, unsigned long now)
  {
    return (long)(untilMs - now) > 0;
  }

  static void updateDirectionGuards(uint16_t exp, unsigned long now)
  {
    bool centerLow = !((exp >> EXP_PIN_ENCODER_BTN) & 0x01);
    int dirsLow = (!((exp >> EXP_PIN_JOY_UP) & 0x01)) + (!((exp >> EXP_PIN_JOY_DOWN) & 0x01)) +
                  (!((exp >> EXP_PIN_JOY_LEFT) & 0x01)) + (!((exp >> EXP_PIN_JOY_RIGHT) & 0x01));

    // Two or more directions at once (diagonal or opposite) is never a
    // deliberate single-cell nudge.
    s_dirsAmbiguous = (dirsLow >= 2);

    if (!centerLow)
    {
      s_centerPureStartMs = 0;
      return;
    }

    if (dirsLow == 0)
    {
      // Center contact closed with no direction: a genuine push, but only
      // trusted once it has persisted briefly - a real tilt can close the
      // center contact a hair before its own direction contact does.
      if (s_centerPureStartMs == 0)
        s_centerPureStartMs = (now != 0) ? now : 1;
      if (now - s_centerPureStartMs >= DIR_CENTER_PURE_MS)
        s_centerGuardUntilMs = now + DIR_BUTTON_LOCKOUT_MS;
    }
    else if (guardActive(s_centerGuardUntilMs, now))
    {
      // Center still held while a direction wobbles in: keep the lockout
      // extended for as long as the push lasts. (With no genuine push
      // already established, center+direction is just the tilt switch's
      // own quirk and must not lock anything out.)
      s_centerGuardUntilMs = now + DIR_BUTTON_LOCKOUT_MS;
    }
  }

  static bool directionsBlocked(unsigned long now)
  {
    return s_dirsAmbiguous ||
           guardActive(s_centerGuardUntilMs, now) ||
           guardActive(s_encoderGuardUntilMs, now);
  }

  // Which guard is currently blocking directions - for the debug log only.
  static const char *blockReason(unsigned long now)
  {
    if (s_dirsAmbiguous)
      return "several directions at once";
    if (guardActive(s_centerGuardUntilMs, now))
      return "center push";
    return "encoder turn";
  }

  static uint16_t expanderState()
  {
    unsigned long now = millis();

    if (!s_expanderReady && (now - s_lastExpanderRetryMs) >= EXPANDER_RETRY_INTERVAL_MS)
    {
      s_lastExpanderRetryMs = now;
      pcf8575Write(0xFFFF); // re-assert input-with-pullup mode on every pin before trusting a read
      if (pcf8575Ping())
      {
        s_expanderReady = true;
        if (s_debugEnabled)
        {
          Serial.println("[input] PCF8575 responded - buttons/joystick now live (no reset needed)");
        }
      }
    }

    if (!s_expanderCacheValid || now != s_expanderCacheMs)
    {
      // While not yet ready, don't even attempt pcf8575Read() - pcf8575Ping()
      // above already owns the "is it there yet" question at a throttled
      // cadence; a bare failed read here would just be a second, redundant
      // bus transaction every single loop() iteration until recovery.
      s_expanderCache = s_expanderReady ? pcf8575Read() : 0xFFFF;
      s_expanderCacheMs = now;
      s_expanderCacheValid = true;
      updateDirectionGuards(s_expanderCache, now);
    }
    return s_expanderCache;
  }

  // ---------------------------------------------------------------------
  // Debounced short/long/double press state machine
  // ---------------------------------------------------------------------
  // Used for both PCF8575 buttons (A, B) and the four joystick directions -
  // all six are the same kind of switch-to-GND closure, just on different
  // expander bits.
  struct ButtonState
  {
    uint8_t expBit;                        // PCF8575 pin (EXP_PIN_*)
    bool lastRawState = HIGH;
    bool debouncedState = HIGH;
    unsigned long lastChangeMs = 0;
    unsigned long pressStartMs = 0;
    bool longPressFired = false;
    bool awaitingSecondClick = false;      // a short-press release is pending,
                                           // waiting to see if a second click follows
    unsigned long firstClickReleaseMs = 0; // when that pending release happened
    unsigned long lastRepeatMs = 0;        // 0 = not currently auto-repeating; see pollButtonRepeat()
    bool tainted = false;                  // directions only: contact closed while a guard was active - must
                                           // open again before it can count (DIR_REQUIRE_RELEASE_AFTER_GUARD)
  };

  static ButtonState s_btnA;
  static ButtonState s_btnB;
  static ButtonState s_joyUp;
  static ButtonState s_joyDown;
  static ButtonState s_joyLeft;
  static ButtonState s_joyRight;

  static ButtonEvent pollButton(ButtonState &btn, bool forceReleased = false)
  {
    bool raw = (expanderState() >> btn.expBit) & 0x01;
    if (forceReleased)
    {
      // Caller has determined this raw press is spurious (e.g. the new
      // joystick hardware closes Button A's contact simultaneously with
      // any direction) - treat it as released before it ever reaches the
      // debounce/press-timing state below, rather than letting a real
      // press start and then get cut short, which would surface as a
      // bogus SHORT_PRESS the instant the direction is released.
      raw = HIGH;
    }
    unsigned long now = millis();

    if (raw != btn.lastRawState)
    {
      if (s_debugEnabled)
      {
        Serial.printf("[input P%d] raw -> %s\n", btn.expBit, raw ? "HIGH" : "LOW");
      }
      btn.lastChangeMs = now;
      btn.lastRawState = raw;
    }

    if ((now - btn.lastChangeMs) > BUTTON_DEBOUNCE_MS && raw != btn.debouncedState)
    {
      btn.debouncedState = raw;
      if (btn.debouncedState == LOW)
      {
        // just pressed
        btn.pressStartMs = now;
        btn.longPressFired = false;
      }
      else
      {
        // just released
        unsigned long heldFor = now - btn.pressStartMs;
        if (!btn.longPressFired)
        {
          if (btn.awaitingSecondClick)
          {
            // This release is the second click of a pair - report it now,
            // no need to wait out the window.
            btn.awaitingSecondClick = false;
            if (s_debugEnabled)
            {
              Serial.printf("[input P%d] DOUBLE_PRESS\n", btn.expBit);
            }
            return ButtonEvent::DOUBLE_PRESS;
          }
          else
          {
            // Could still turn into a double-click - hold it back and see
            // what happens within DOUBLE_CLICK_WINDOW_MS.
            btn.awaitingSecondClick = true;
            btn.firstClickReleaseMs = now;
          }
        }
      }
    }

    // Fire long-press the moment the threshold is crossed while still held.
    if (btn.debouncedState == LOW && !btn.longPressFired &&
        (now - btn.pressStartMs) >= LONG_PRESS_MS)
    {
      btn.longPressFired = true;
      btn.awaitingSecondClick = false; // a long press cancels any pending pairing wait
      if (s_debugEnabled)
      {
        Serial.printf("[input P%d] LONG_PRESS (held, still down)\n", btn.expBit);
      }
      return ButtonEvent::LONG_PRESS;
    }

    // No second click showed up in time - the held-back release was just a
    // genuine single click.
    if (btn.awaitingSecondClick && (now - btn.firstClickReleaseMs) > DOUBLE_CLICK_WINDOW_MS)
    {
      btn.awaitingSecondClick = false;
      if (s_debugEnabled)
      {
        Serial.printf("[input P%d] SHORT_PRESS (held %lums)\n", btn.expBit, now - btn.firstClickReleaseMs);
      }
      return ButtonEvent::SHORT_PRESS;
    }

    return ButtonEvent::NONE;
  }

  // The RKJXT1F42001's 4-direction tilt switch also closes the center
  // push (Button A) contact whenever any direction is actuated - a
  // hardware quirk of this part, not a real simultaneous press. Rather
  // than let that spurious closure feed the debounce/short/long/double
  // press machine at all, it's masked out here: if any direction bit is
  // currently low (pressed) in this same expander snapshot, Button A's
  // raw reading is forced to "released" before pollButton() ever sees
  // it. Directions themselves go through pollDirection() below and are
  // unaffected.
  static bool anyDirectionRawPressed()
  {
    uint16_t exp = expanderState();
    return !((exp >> EXP_PIN_JOY_UP) & 0x01) ||
           !((exp >> EXP_PIN_JOY_DOWN) & 0x01) ||
           !((exp >> EXP_PIN_JOY_LEFT) & 0x01) ||
           !((exp >> EXP_PIN_JOY_RIGHT) & 0x01);
  }

  // s_btnA is the physical pin with the direction-quirk masking (see
  // anyDirectionRawPressed() above) - that's a property of the encoder's
  // own center-push pin, so it stays applied to s_btnA regardless of
  // which logical role (A or B) is currently reading it.
  ButtonEvent pollButtonA()
  {
    return s_buttonsSwapped ? pollButton(s_btnB) : pollButton(s_btnA, anyDirectionRawPressed());
  }
  ButtonEvent pollButtonB()
  {
    return s_buttonsSwapped ? pollButton(s_btnA, anyDirectionRawPressed()) : pollButton(s_btnB);
  }

  // Raw physical-pin checks (see InputManager.h). Each call uses the shared
  // once-per-millisecond expander snapshot, so it's cheap to poll in a loop.
  bool bothButtonsHeld()
  {
    uint16_t exp = expanderState();
    bool a = !((exp >> EXP_PIN_ENCODER_BTN) & 0x01) && !anyDirectionRawPressed();
    bool b = !((exp >> EXP_PIN_BUTTON2) & 0x01);
    return a && b;
  }

  bool anyButtonHeld()
  {
    uint16_t exp = expanderState();
    bool a = !((exp >> EXP_PIN_ENCODER_BTN) & 0x01) && !anyDirectionRawPressed();
    bool b = !((exp >> EXP_PIN_BUTTON2) & 0x01);
    return a || b;
  }

  // Direction switches (joystick tilt) are always instant, single-shot
  // presses - unlike buttons A/B, there's no long-press or double-press
  // meaning on a direction, so this doesn't run through pollButton()'s
  // debounced short/long/double-press machine at all. In particular it
  // never holds a release back to see if a second click follows
  // (awaitingSecondClick's DOUBLE_CLICK_WINDOW_MS wait) and never starts
  // a long-press timer - it just reports SHORT_PRESS the instant the
  // debounced state flips to pressed, one event per physical press, with
  // none of that added latency. It still updates the same
  // debouncedState/pressStartMs fields pollButton() would have, so
  // pollButtonRepeat() (press-and-hold auto-repeat, used for these same
  // four directions) keeps working unchanged.
  static ButtonEvent pollDirection(ButtonState &btn)
  {
    bool raw = (expanderState() >> btn.expBit) & 0x01;
    unsigned long now = millis();

    // Accidental-nudge guards (encoder activity, center push, multiple
    // directions - see updateDirectionGuards()): treat the direction as
    // released before it reaches the debounce state, same approach as
    // pollButton()'s forceReleased.
    const bool rawPhys = raw;
    if (directionsBlocked(now))
    {
      if (rawPhys == LOW)
      {
        if (!btn.tainted && s_debugEnabled)
        {
          Serial.printf("[input P%d] direction ignored (%s)\n", btn.expBit, blockReason(now));
        }
        btn.tainted = true;
      }
      raw = HIGH;
    }
    else if (btn.tainted)
    {
      // Guard just ended. If the contact is still closed it's the same
      // wobble that started inside the guard - keep ignoring it until it
      // opens (or, with the feature off, let it through as before).
      if (rawPhys == HIGH)
      {
        btn.tainted = false;
      }
      else
      {
#if DIR_REQUIRE_RELEASE_AFTER_GUARD
        raw = HIGH;
#else
        btn.tainted = false;
#endif
      }
    }

    if (raw != btn.lastRawState)
    {
      if (s_debugEnabled)
      {
        Serial.printf("[input P%d] raw -> %s\n", btn.expBit, raw ? "HIGH" : "LOW");
      }
      btn.lastChangeMs = now;
      btn.lastRawState = raw;
    }

    if ((now - btn.lastChangeMs) > BUTTON_DEBOUNCE_MS && raw != btn.debouncedState)
    {
      btn.debouncedState = raw;
      if (btn.debouncedState == LOW)
      {
        // Just pressed - not reported yet; it has to survive
        // DIR_MIN_HOLD_MS first (below). longPressFired is reused here as
        // "this press has already been reported".
        btn.pressStartMs = now;
        btn.longPressFired = false;
      }
      else
      {
        btn.longPressFired = false;
      }
    }

    // Report the press once it has been held long enough to rule out a
    // brush/wobble. One event per physical press; release isn't reported.
    if (btn.debouncedState == LOW && !btn.longPressFired &&
        (now - btn.pressStartMs) >= s_dirMinHoldMs)
    {
      btn.longPressFired = true;
      if (s_debugEnabled)
      {
        Serial.printf("[input P%d] SHORT_PRESS (direction, held %lums)\n", btn.expBit, now - btn.pressStartMs);
      }
      return ButtonEvent::SHORT_PRESS;
    }

    return ButtonEvent::NONE;
  }

  ButtonEvent pollJoyUp() { return pollDirection(s_joyUp); }
  ButtonEvent pollJoyDown() { return pollDirection(s_joyDown); }
  ButtonEvent pollJoyLeft() { return pollDirection(s_joyLeft); }
  ButtonEvent pollJoyRight() { return pollDirection(s_joyRight); }

  // Press-and-hold auto-repeat. Reads btn.debouncedState/pressStartMs,
  // which pollButton() above is responsible for keeping current - callers
  // must poll the matching pollJoy*() every tick regardless of whether
  // its return value is used, same requirement as the rest of this file.
  //
  // lastRepeatMs doing double duty as "haven't started repeating yet"
  // (0) and "timestamp of the last repeat fired" is safe here because a
  // genuine repeat timestamp of exactly 0 would require millis() to have
  // wrapped back to 0 at the exact instant a repeat fired - not worth a
  // separate bool for.
  static bool pollButtonRepeat(ButtonState &btn)
  {
    unsigned long now = millis();

    if (btn.debouncedState != LOW)
    {
      btn.lastRepeatMs = 0; // released (or never pressed) - next press starts a fresh delay
      return false;
    }

    unsigned long heldFor = now - btn.pressStartMs;
    if (heldFor < MOVE_REPEAT_DELAY_MS)
    {
      return false;
    }

    if (btn.lastRepeatMs == 0)
    {
      // Anchor the first repeat to pressStartMs + delay, not to "now" -
      // otherwise the first repeat's gap from the initial press would be
      // delay + (however late this particular tick happened to land),
      // and every repeat after it would inherit that same drift.
      btn.lastRepeatMs = btn.pressStartMs + MOVE_REPEAT_DELAY_MS;
    }

    if (now - btn.lastRepeatMs < MOVE_REPEAT_INTERVAL_MS)
    {
      return false;
    }

    btn.lastRepeatMs = now;
    if (s_debugEnabled)
    {
      Serial.printf("[input P%d] REPEAT (held %lums)\n", btn.expBit, heldFor);
    }
    return true;
  }

  bool pollJoyUpRepeat() { return pollButtonRepeat(s_joyUp); }
  bool pollJoyDownRepeat() { return pollButtonRepeat(s_joyDown); }
  bool pollJoyLeftRepeat() { return pollButtonRepeat(s_joyLeft); }
  bool pollJoyRightRepeat() { return pollButtonRepeat(s_joyRight); }

  // Best-effort I2C bus recovery, run before Wire.begin() claims the pins.
  // If a previous session ended mid-transaction (e.g. a reset while a
  // transfer was in flight, or the PCF8575 wasn't fully powered up yet
  // when an earlier transaction hit it on a cold boot), a bit-banged I2C
  // bus can get left with SDA stuck low - the classic symptom being every
  // transaction failing silently forever afterward, until something
  // forces a clean re-init.
  //


  void begin()
  {
    pinMode(PIN_ENCODER_CLK, INPUT_PULLUP);
    pinMode(PIN_ENCODER_DT, INPUT_PULLUP);

    Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);

    // GPIO16 can't do attachInterrupt() (see config.h), so /INT is only
    // usable as a polled digitalRead(). It's not needed for correctness at
    // the current loop cadence (see expanderState() above) - left as a
    // plain input here so it's available if a future power-saving pass
    // wants it.
    pinMode(PIN_EXPANDER_INT, INPUT);

    // Quasi-bidirectional PCF8575: writing 1 to every pin is what puts them
    // all in input-with-pull-up mode (also the power-on default, but made
    // explicit here rather than relied upon). Retried with verification
    // (pcf8575Ping(), not pcf8575Read() - see its comment on why that
    // distinction matters here) rather than fired once and assumed to have
    // worked: on a cold power-on, GPIO0/GPIO2 doubling as both I2C pins and
    // boot-strap pins means the PCF8575's own power/reset sequencing can
    // still be settling by the time this runs, even after setup()'s
    // earlier delay(200). A handful of retries with a short pause between
    // them covers that without needing a guessed-at fixed delay - and a
    // warm reset (e.g. triggered by a serial monitor's DTR/RTS toggle,
    // which only resets the MCU, not the already-stable rest of the
    // board) never hits this race in the first place, which is the
    // telltale sign of this exact issue.
    bool expanderReady = false;
    for (int attempt = 0; attempt < 5 && !expanderReady; attempt++)
    {
      pcf8575Write(0xFFFF);
      expanderReady = pcf8575Ping();
      if (!expanderReady)
        delay(50);
    }
    s_expanderReady = expanderReady;
    s_lastExpanderRetryMs = millis();
    if (!expanderReady)
    {
      // Printed unconditionally (not gated on s_debugEnabled) since this
      // is worth surfacing even with debug output off. No longer a
      // permanent condition, though - expanderState() keeps retrying
      // pcf8575Ping() quietly every EXPANDER_RETRY_INTERVAL_MS from here
      // on, so buttons/joystick will start working on their own as soon
      // as the expander answers, with no reset required. If this message
      // is followed by "PCF8575 responded" a few seconds later, that's
      // just slow expander/bus settling on cold power-on and nothing to
      // worry about; if it never recovers, check wiring/power to the
      // expander and the pull-ups on SDA/SCL.
      if (s_debugEnabled)
      {
        Serial.println("[input] WARNING: PCF8575 did not respond during init after 5 attempts - "
                       "will keep retrying in the background.");
      }
    }

    s_btnA.expBit = EXP_PIN_ENCODER_BTN;
    s_btnB.expBit = EXP_PIN_BUTTON2;
    s_joyUp.expBit = EXP_PIN_JOY_UP;
    s_joyDown.expBit = EXP_PIN_JOY_DOWN;
    s_joyLeft.expBit = EXP_PIN_JOY_LEFT;
    s_joyRight.expBit = EXP_PIN_JOY_RIGHT;

    s_lastAB = (digitalRead(PIN_ENCODER_CLK) << 1) | digitalRead(PIN_ENCODER_DT);

    attachInterrupt(digitalPinToInterrupt(PIN_ENCODER_CLK), encoderISR, CHANGE);
    attachInterrupt(digitalPinToInterrupt(PIN_ENCODER_DT), encoderISR, CHANGE);

    if (s_debugEnabled)
    {
      Serial.printf("[input] CLK=GPIO%d DT=GPIO%d (native, interrupt driven)\n",
                    PIN_ENCODER_CLK, PIN_ENCODER_DT);
      Serial.printf("[input] PCF8575 0x%02X on SDA=GPIO%d SCL=GPIO%d: BTN_A=P%d JOY_UP=P%d JOY_DOWN=P%d JOY_LEFT=P%d JOY_RIGHT=P%d BTN_B=P%d\n",
                    PCF8575_I2C_ADDR, PIN_I2C_SDA, PIN_I2C_SCL,
                    EXP_PIN_ENCODER_BTN, EXP_PIN_JOY_UP, EXP_PIN_JOY_DOWN,
                    EXP_PIN_JOY_LEFT, EXP_PIN_JOY_RIGHT, EXP_PIN_BUTTON2);
      Serial.printf("[input] expander responded to init ping: %s\n", expanderReady ? "yes" : "no");
      uint16_t initial = expanderState();
      Serial.printf("[input] initial expander state = 0x%04X (bits for pins above should read 1/HIGH if idle and wired correctly)\n",
                    initial);
    }
  }

} // namespace InputManager