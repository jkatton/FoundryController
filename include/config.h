#pragma once


// =====================================================================
// PIN CONFIGURATION - Wemos D1 Mini (ESP8266)
// =====================================================================
// Controller updated to the Alps Alpine RKJXT1F42001 (encoder + center
// push + 4-direction tilt switch, 7 signals total instead of the old
// encoder's 3), which no longer fits on the D1 Mini's 9 native pins
// alongside the TFT. Split across native GPIO + a PCF8575 I2C expander:
//
//   Native (still needs to be fast/interrupt-driven or is fixed by
//   hardware):
//     - Encoder CLK/DT: quadrature decoding is timing-sensitive: I2C
//       polling latency would miss or misread fast rotations, so these
//       stay on native GPIO with real interrupts, same as before.
//     - TFT MOSI/SCLK/CS/DC: toggled per-byte during every SPI
//       transfer; routing these through I2C would be orders of
//       magnitude too slow for the display. MOSI/SCLK are also fixed
//       hardware SPI pins on the ESP8266 regardless.
//     - I2C SDA/SCL: needed to reach the PCF8575 at all.
//
//   Moved to the PCF8575 (slow, simple switch closures - a good fit for
//   quasi-bidirectional I2C inputs, no external pull-ups needed):
//     - Encoder center push, joystick Up/Down/Left/Right, Button B.
//
// TFT_RST is no longer wired to a GPIO at all - tied straight to 3V3
// like the backlight already was, with the display driver issuing a
// software reset (SWRESET) at init instead. That's what frees up a 9th
// native pin for I2C without needing to put anything timing-sensitive
// on the expander.
//
// Two hard constraints on the ESP8266, unlike the ESP32:
//  1. SPI MOSI/SCLK are NOT freely reassignable - hardware SPI only works
//     on GPIO13 (MOSI/D7) and GPIO14 (SCLK/D5). TFT_MOSI/TFT_SCLK below
//     must stay on those pins.
//  2. GPIO16 (D0) has no interrupt support and no usable internal
//     pull-up. It's used here for the PCF8575's /INT line, but that
//     means /INT must be polled (digitalRead in the main loop) rather
//     than handled via attachInterrupt - GPIO16 can't do the latter.
//
// D3 (GPIO0) and D4 (GPIO2) are boot-strapping pins - they must read HIGH
// at power-on/reset for the board to boot normally. They're used here for
// I2C SDA/SCL, which works out nicely: I2C requires pull-up resistors on
// both lines (either external ~4.7k or built into most PCF8575 breakout
// boards), and those pull-ups naturally hold the lines HIGH at boot,
// satisfying the boot-strap requirement the same way the old buttons'
// INPUT_PULLUP did. As before: don't power on mid-press of anything that
// could pull these low.

// Rotary encoder (KY-040-style quadrature phases, now from the
// RKJXT1F42001 rather than a standalone encoder)
#define PIN_ENCODER_CLK     5   // D1
#define PIN_ENCODER_DT      4   // D2

// I2C bus to the PCF8575 expander
#define PIN_I2C_SDA         0   // D3 - external/onboard pull-up also satisfies boot-strap requirement
#define PIN_I2C_SCL         2   // D4 - external/onboard pull-up also satisfies boot-strap requirement

// PCF8575 /INT - goes low when any expander input changes. GPIO16 can't
// do attachInterrupt(), so this must be polled with digitalRead() rather
// than used as a true interrupt.
#define PIN_EXPANDER_INT    16  // D0

// TFT (GC9A01 round 240x240 SPI display) - these must also match the
// build_flags in platformio.ini, which is what TFT_eSPI actually uses.
#define PIN_TFT_MOSI        13   // D7 - fixed hardware SPI pin, do not move
#define PIN_TFT_SCLK        14   // D5 - fixed hardware SPI pin, do not move
#define PIN_TFT_CS          15   // D8
#define PIN_TFT_DC          12   // D6
#define PIN_TFT_RST         -1   // tied directly to 3V3 - display driver does a software reset (SWRESET) instead
#define PIN_TFT_BL          -1   // tied directly to 3V3 - no spare GPIO for backlight control

// =====================================================================
// PCF8575 I2C EXPANDER
// =====================================================================
// 16 quasi-bidirectional I/O pins (P0-P15), used here for everything on
// the controller that isn't timing-sensitive: the encoder's center push,
// the RKJXT1F42001's 4-direction tilt switch, and the standalone Button
// B. All are simple switch-to-GND closures, same debounce approach as
// before (BUTTON_DEBOUNCE_MS) - just read over I2C instead of digitalRead().
#define PCF8575_I2C_ADDR       0x20   // default address with all address pins tied low; adjust if wired differently

#define EXP_PIN_ENCODER_BTN    0   // P0 - RKJXT1F42001 center push ("Button A")
#define EXP_PIN_JOY_UP         1   // P1
#define EXP_PIN_JOY_DOWN       2   // P2
#define EXP_PIN_JOY_LEFT       3   // P3
#define EXP_PIN_JOY_RIGHT      4   // P4
#define EXP_PIN_BUTTON2        5   // P5 - standalone "Button B"

// =====================================================================
// TIMING
// =====================================================================
#define LONG_PRESS_MS           600     // ms held before a press counts as "long"
#define BUTTON_DEBOUNCE_MS       25
#define ENCODER_POLL_MS           2     // encoder is interrupt driven, this is just the ISR debounce guard


#define RELAY_TEST_TIMEOUT_MS   4000    // timeout when testing a saved/entered address
#define RELAY_HTTP_TIMEOUT_MS   5000
#define CHARACTER_POLL_MS       5000    // how often to refresh character list / hp while viewing it
#define WIFI_PORTAL_TIMEOUT_S    180    // WiFiManager config portal timeout (seconds)

// Max gap between the release of the first click and the press of the
// second for pollButtonA()/pollButtonB() to report DOUBLE_PRESS instead of
// two separate SHORT_PRESS events. Every genuine single click now waits up
// to this long before being reported, so keep it as short as feels
// reliable - 250-350ms is typical for a deliberate double-click.
#define DOUBLE_CLICK_WINDOW_MS 300


// pre-fill the Port the relay server listens on (default is 8787).
#define RELAY_DEFAULT_PORT 8787


// LittleFS path used to remember the relay address across reboots
#define RELAY_CONFIG_PATH "/relay.cfg"

// LittleFS path used to remember user settings (screen rotation, gauge
// style, ...) across reboots - see Storage::loadSettings/saveSettings and
// the settings carousel reached by long-pressing button B from the IP
// entry screen. AppSettings' member defaults (Types.h) are what's used
// the first time this file doesn't exist yet.
#define SETTINGS_CONFIG_PATH "/settings.cfg"

// =====================================================================
// DISPLAY
// =====================================================================
#define SCREEN_W 240
#define SCREEN_H 240
#define SCREEN_CX 120
#define SCREEN_CY 120
