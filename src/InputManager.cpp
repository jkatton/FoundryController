#include "InputManager.h"
#include "config.h"

namespace InputManager
{

  // Runtime-editable (see setDebugEnabled()/debugEnabled(), and the
  // "Serial Debug Output" entry on the settings screen) - defaults to
  // INPUT_DEBUG from config.h. Replaces what used to be a compile-time
  // #if INPUT_DEBUG guard around every Serial.printf() below.
  static bool s_debugEnabled = false;

  void setDebugEnabled(bool enabled) { s_debugEnabled = enabled; }
  bool debugEnabled() { return s_debugEnabled; }

  // ---------------------------------------------------------------------
  // Rotary encoder - interrupt driven quadrature decode
  // ---------------------------------------------------------------------
  static volatile int32_t s_rawCounter = 0;
  static int32_t s_lastProcessed = 0;
  static volatile uint8_t s_lastAB = 0;
  static unsigned long s_lastStepMs = 0; // timestamp of last nonzero step, for acceleration

  static const int8_t QUAD_TABLE[16] = {
      0, -1, +1, 0,
      +1, 0, 0, -1,
      -1, 0, 0, +1,
      0, +1, -1, 0};

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
  }

  int32_t readEncoderDelta(EncoderProfile profile)
  {
    noInterrupts();
    int32_t current = s_rawCounter;
    interrupts();

    int32_t diff = current - s_lastProcessed;
    int32_t steps = diff / 4;
    s_lastProcessed += steps * 4;

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
  // Debounced short/long press button state machine
  // ---------------------------------------------------------------------
  struct ButtonState
  {
    uint8_t pin;
    bool lastRawState = HIGH;
    bool debouncedState = HIGH;
    unsigned long lastChangeMs = 0;
    unsigned long pressStartMs = 0;
    bool longPressFired = false;
    bool awaitingSecondClick = false;      // a short-press release is pending,
                                           // waiting to see if a second click follows
    unsigned long firstClickReleaseMs = 0; // when that pending release happened
  };

  static ButtonState s_btnA;
  static ButtonState s_btnB;

  static ButtonEvent pollButton(ButtonState &btn)
  {
    bool raw = digitalRead(btn.pin);
    unsigned long now = millis();

    if (raw != btn.lastRawState)
    {
      if (s_debugEnabled)
      {
        Serial.printf("[button pin %d] raw -> %s\n", btn.pin, raw ? "HIGH" : "LOW");
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
              Serial.printf("[button pin %d] DOUBLE_PRESS\n", btn.pin);
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
        Serial.printf("[button pin %d] LONG_PRESS (held, still down)\n", btn.pin);
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
        Serial.printf("[button pin %d] SHORT_PRESS (held %lums)\n", btn.pin, now - btn.firstClickReleaseMs);
      }
      return ButtonEvent::SHORT_PRESS;
    }

    return ButtonEvent::NONE;
  }

  ButtonEvent pollButtonA() { return pollButton(s_btnA); }
  ButtonEvent pollButtonB() { return pollButton(s_btnB); }

  void begin()
  {
    pinMode(PIN_ENCODER_CLK, INPUT_PULLUP);
    pinMode(PIN_ENCODER_DT, INPUT_PULLUP);
    pinMode(PIN_ENCODER_BTN, INPUT_PULLUP);
    pinMode(PIN_BUTTON2, INPUT_PULLUP);

    s_btnA.pin = PIN_ENCODER_BTN;
    s_btnB.pin = PIN_BUTTON2;

    s_lastAB = (digitalRead(PIN_ENCODER_CLK) << 1) | digitalRead(PIN_ENCODER_DT);

    attachInterrupt(digitalPinToInterrupt(PIN_ENCODER_CLK), encoderISR, CHANGE);
    attachInterrupt(digitalPinToInterrupt(PIN_ENCODER_DT), encoderISR, CHANGE);

    if (s_debugEnabled)
    {
      Serial.printf("[input] CLK=GPIO%d DT=GPIO%d BTN_A=GPIO%d BTN_B=GPIO%d\n",
                    PIN_ENCODER_CLK, PIN_ENCODER_DT, PIN_ENCODER_BTN, PIN_BUTTON2);
      Serial.printf("[input] initial CLK=%d DT=%d BTN_A=%d BTN_B=%d (all should read 1/HIGH if idle and wired correctly)\n",
                    digitalRead(PIN_ENCODER_CLK), digitalRead(PIN_ENCODER_DT),
                    digitalRead(PIN_ENCODER_BTN), digitalRead(PIN_BUTTON2));
    }
  }

} // namespace InputManager
