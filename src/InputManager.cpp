#include "InputManager.h"
#include "config.h"
#include <Wire.h>

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
  // Unchanged by the RKJXT1F42001 switch: CLK/DT stay on native GPIO (see
  // config.h for why quadrature decoding can't move to the I2C expander).
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

  static uint16_t expanderState()
  {
    unsigned long now = millis();
    if (!s_expanderCacheValid || now != s_expanderCacheMs)
    {
      s_expanderCache = pcf8575Read();
      s_expanderCacheMs = now;
      s_expanderCacheValid = true;
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
  };

  static ButtonState s_btnA;
  static ButtonState s_btnB;
  static ButtonState s_joyUp;
  static ButtonState s_joyDown;
  static ButtonState s_joyLeft;
  static ButtonState s_joyRight;

  static ButtonEvent pollButton(ButtonState &btn)
  {
    bool raw = (expanderState() >> btn.expBit) & 0x01;
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

  ButtonEvent pollButtonA() { return pollButton(s_btnA); }
  ButtonEvent pollButtonB() { return pollButton(s_btnB); }

  ButtonEvent pollJoyUp() { return pollButton(s_joyUp); }
  ButtonEvent pollJoyDown() { return pollButton(s_joyDown); }
  ButtonEvent pollJoyLeft() { return pollButton(s_joyLeft); }
  ButtonEvent pollJoyRight() { return pollButton(s_joyRight); }

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
    // explicit here rather than relied upon).
    pcf8575Write(0xFFFF);

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
      uint16_t initial = expanderState();
      Serial.printf("[input] initial expander state = 0x%04X (bits for pins above should read 1/HIGH if idle and wired correctly)\n",
                    initial);
    }
  }

} // namespace InputManager