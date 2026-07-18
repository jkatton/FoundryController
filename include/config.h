#pragma once


// =====================================================================
// PIN CONFIGURATION - Wemos D1 Mini (ESP8266)
// =====================================================================
// The D1 Mini exposes 9 usable digital pins (D0-D8), and this project
// needs exactly that many (encoder CLK/DT/SW, button B, TFT MOSI/SCLK/
// CS/DC/RST) - so the backlight is wired straight to 3V3 instead of a
// GPIO, and there's no pin to spare.
//
// Two hard constraints on the ESP8266, unlike the ESP32:
//  1. SPI MOSI/SCLK are NOT freely reassignable - hardware SPI only works
//     on GPIO13 (MOSI/D7) and GPIO14 (SCLK/D5). TFT_MOSI/TFT_SCLK below
//     must stay on those pins.
//  2. GPIO16 (D0) has no interrupt support and no usable internal
//     pull-up, so it can't be used for the encoder or a pulled-up button.
//     It's used here for TFT_RST instead (a plain output, which is fine).
//
// D3 (GPIO0) and D4 (GPIO2) are boot-strapping pins - they must read HIGH
// at power-on/reset for the board to boot normally. Both are used for
// buttons here (idle-high via INPUT_PULLUP, only pulled low when
// pressed), which is the same pattern the D1 Mini's own onboard "FLASH"
// button uses on D3 - but if you happen to be holding a button down at
// the exact moment you power on or reset the board, it can drop into the
// bootloader instead of booting normally. Just don't power on mid-press.
// D4 also drives the onboard blue LED (active-low), so it'll blink
// faintly whenever Button B is held - cosmetic only.

// Rotary encoder (KY-040 style quadrature encoder)
#define PIN_ENCODER_CLK     5   // D1
#define PIN_ENCODER_DT      4   // D2
#define PIN_ENCODER_BTN     0   // D3 - integrated push-button on the encoder shaft ("Button A")

// Second standalone button ("Button B")
#define PIN_BUTTON2          2   // D4

// TFT (GC9A01 round 240x240 SPI display) - these must also match the
// build_flags in platformio.ini, which is what TFT_eSPI actually uses.
#define PIN_TFT_MOSI        13   // D7 - fixed hardware SPI pin, do not move
#define PIN_TFT_SCLK        14   // D5 - fixed hardware SPI pin, do not move
#define PIN_TFT_CS          15   // D8
#define PIN_TFT_DC          12   // D6
#define PIN_TFT_RST         16   // D0
#define PIN_TFT_BL          -1   // tied directly to 3V3 - no spare GPIO for backlight control

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

