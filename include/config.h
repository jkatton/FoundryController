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
// Three hard constraints on the ESP8266, unlike the ESP32:
//  1. SPI MOSI/SCLK are NOT freely reassignable - hardware SPI only works
//     on GPIO13 (MOSI/D7) and GPIO14 (SCLK/D5). TFT_MOSI/TFT_SCLK below
//     must stay on those pins.
//  2. GPIO16 (D0) lives in the RTC/deep-sleep hardware block rather
//     than the normal GPIO matrix, and - confirmed by direct testing on
//     this board, not just datasheet theory - the Arduino Wire library
//     simply cannot drive it for I2C, as either SDA or SCL. Used here
//     only for the (currently unused/spare) PCF8575 /INT line, which
//     just needs plain digitalRead() - no interrupt, no I2C.
//  3. GPIO0 (D3) is wired, on most USB-serial adapter boards, into the
//     auto-reset circuit that the flashing tool uses to force bootloader
//     mode (driven via the adapter's RTS line through a transistor).
//     Whenever nothing has the COM port open, that transistor commonly
//     idles in the conducting state, holding GPIO0 low continuously -
//     which would be fatal to I2C on this pin if true. This is the
//     leading theory for why I2C here has worked only while a serial
//     monitor is attached, but as of this writing it's still pending
//     confirmation with a multimeter (checking D3's voltage with the
//     monitor open vs. closed) before treating it as settled - flagged
//     here rather than silently assumed. If confirmed, the actual fix
//     is a small hardware mod disabling that auto-reset connection
//     (trades away one-click auto-flash for a manual boot-button
//     sequence), not a firmware workaround - the external transistor
//     can't be out-retried by software.
//
// D4 (GPIO2) is a boot-strapping pin - it must read HIGH at power-on/
// reset for the board to boot normally. It's used here for I2C SCL,
// which works out nicely: I2C requires a pull-up resistor on the line
// (either external ~4.7k or built into most PCF8575 breakout boards),
// and that pull-up naturally holds the line HIGH at boot, satisfying the
// boot-strap requirement the same way the old buttons' INPUT_PULLUP did.
// As before: don't power on mid-press of anything that could pull it low.

// Rotary encoder (KY-040-style quadrature phases, now from the
// RKJXT1F42001 rather than a standalone encoder)
#define PIN_ENCODER_CLK     5   // D1
#define PIN_ENCODER_DT      4   // D2

// I2C bus to the PCF8575 expander
//
// SDA=GPIO0/SCL=GPIO2 is the original pairing, confirmed by direct
// testing to be the only combination on this board that actually talks
// to the PCF8575 at all - a detour through GPIO16 for either line was
// tried and confirmed dead (see constraint #2 above), and GPIO0 alone
// is not swappable for GPIO16 either, for the same reason. So this pair
// stays, and the remaining "only works with a monitor attached" issue
// (constraint #3 above) needs to be solved by fixing GPIO0's contention
// at its source rather than avoiding the pin.
#define PIN_I2C_SDA         0   // D3 - see constraint #3 above re: USB auto-reset contention
#define PIN_I2C_SCL         2   // D4 - external/onboard pull-up also satisfies boot-strap requirement

// PCF8575 /INT - goes low when any expander input changes. Currently
// unused (nothing calls digitalRead() on it - see InputManager.cpp) and
// GPIO16 can't do attachInterrupt() anyway, so this is a plain polled
// spare pin either way.
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
#define EXP_PIN_JOY_UP         3   // P1
#define EXP_PIN_JOY_DOWN       1   // P2 
#define EXP_PIN_JOY_LEFT       4   // P3 
#define EXP_PIN_JOY_RIGHT      2   // P4
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

// Press-and-hold auto-repeat for the joystick directions (used by move
// mode - see main.cpp's nudgeMove()/nudgeMoveDuringCircleMode() - to add
// movement continuously while a direction is held rather than requiring
// one press per cell). REPEAT_DELAY is how long a direction must be held
// before repeating starts; REPEAT_INTERVAL is the gap between repeats
// after that. Kept slower than a typical keyboard repeat since each
// repeat fires a blocking network preview call (see sendMovePreview()) -
// too fast and holding a direction floods the relay with requests instead
// of smoothly stepping the preview.
#define MOVE_REPEAT_DELAY_MS    500
#define MOVE_REPEAT_INTERVAL_MS 250


// pre-fill the Port the relay server listens on (default is 8787).
#define RELAY_DEFAULT_PORT 8787

// Circle mode (opened with a held Button B from the character detail
// screen - see main.cpp's g_circleMode): encoder step size and bounds for
// the radius, in the scene's own distance units (feet, meters, ...).
// Defaults match dnd5e's typical 5ft grid squares / spell radius
// increments.
#define CIRCLE_RADIUS_STEP     5
#define CIRCLE_RADIUS_MIN      5
#define CIRCLE_RADIUS_MAX      240
#define CIRCLE_RADIUS_DEFAULT  60

// How long the encoder must sit still after a radius change before that
// radius actually gets pushed to the relay as a live preview - each
// encoder tick still updates the on-screen number immediately, but the
// (blocking, network-round-trip) preview send is coalesced to fire once
// after things settle, rather than once per tick while spinning fast.
#define CIRCLE_RADIUS_DEBOUNCE_MS 100



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

// Single-character labels used to build the on-screen button hint text
// (e.g. "<A>:next", "[B]:back" - see Display.cpp's shortLabel()/
// longLabel()/doubleLabel() helpers). Defined here, in one place, rather
// than hardcoded inline throughout Display.cpp's hint strings, purely so
// a future relabeling (different silkscreen, different physical buttons)
// only requires changing these two lines.
//
// This is cosmetic only - it does NOT control which physical button does
// what. That's AppSettings::swapButtons (Types.h) and
// InputManager::setButtonsSwapped(); Display.cpp combines the two,
// picking whichever of these two labels currently names the physical
// button performing a given role (see Display.cpp's labelA()/labelB()).
//
// These are currently set to two custom icon glyphs rather than plain
// letters - codes 0x60 (backtick) and 0x7E (tilde) of TFT_eSPI's default
// GLCD font (font 1, the only font this project uses), which
// scripts/patch_glcdfont.py overwrites with an X / circle icon on every
// build (see that file for the actual bit patterns). Neither backtick
// nor tilde appears anywhere in this codebase's on-screen text, which is
// what makes them safe to repurpose.
//
// Deliberately NOT one of the control-character codes (1-31) that seem
// like the obvious "unused" slots at first glance: TFT_eSPI's low-level
// glyph-drawing routine (TFT_eSPI::drawChar(x, y, c, color, bg, size))
// unconditionally does `if (c < 32) return;` before it ever looks at the
// font table - it reserves layout width for those codes but never
// actually paints them, regardless of font. Codes 32 and up don't hit
// that check.
//
// If scripts/patch_glcdfont.py isn't wired up yet (see platformio.ini's
// extra_scripts), these just show the font's original backtick/tilde
// glyphs instead of icons - switch back to plain 'A'/'B' below at any
// time if that script isn't in use.
#define BUTTON_A_LABEL (char)0x60  // backtick - patched to an X
#define BUTTON_B_LABEL (char)0x7E  // tilde - patched to a circle