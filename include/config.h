#pragma once

// Firmware version, shown on the Firmware Update (receive) screen and the
// Send Firmware screens so you can tell units apart and confirm an update
// took. Bump this by hand whenever you make a build you intend to distribute.
#define FIRMWARE_VERSION "1.2.0"

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

// Accidental-nudge guards for the joystick directions. The RKJXT1F42001 is
// a tilt switch, so turning or pressing the encoder can wobble the stick
// enough to briefly close a direction contact. See InputManager.cpp's
// direction guards for how these are applied; tune them on the hardware.
//
// DIR_ENCODER_LOCKOUT_MS: directions are ignored for this long after the
//   last encoder quadrature edge.
// DIR_BUTTON_LOCKOUT_MS: directions are ignored while the center push is
//   held, and for this long after it is released.
// DIR_CENTER_PURE_MS: the center push only counts as a genuine push (for
//   the lockout above) once it has been closed with no direction closed
//   for this long - a real tilt also closes the center contact (hardware
//   quirk) and must not lock itself out.
// DIR_MIN_HOLD_MS: a direction must stay pressed (after BUTTON_DEBOUNCE_MS)
//   for this long before it counts as a nudge. Adds latency to every
//   deliberate nudge, so keep it as small as still rejects brushes.
// Also rejected: two or more directions closed at once (diagonals/opposites).
#define DIR_ENCODER_LOCKOUT_MS  200
#define DIR_BUTTON_LOCKOUT_MS   200
#define DIR_CENTER_PURE_MS       15
#define DIR_MIN_HOLD_MS          60

// Extra protection against *starting* a move by accident. Opening move mode
// is the costly case (it sends a network preview and takes over the screen),
// while nudges once move mode is already open can stay quick. So while on the
// character detail screen with no move in progress, a direction must be held
// for DIR_START_HOLD_MS instead of DIR_MIN_HOLD_MS before it counts. Once
// move mode is open the shorter DIR_MIN_HOLD_MS applies again. Set equal to
// DIR_MIN_HOLD_MS to disable.
#define DIR_START_HOLD_MS       150

// A direction contact that was already closed while one of the guards above
// was active (encoder just turned, center push, several directions at once)
// is treated as a wobble and must open again before it can count. Without
// this, a wobble that outlasts the lockout window would register the moment
// the lockout expired. Side effect: a deliberate tilt that starts inside a
// lockout window (e.g. within DIR_ENCODER_LOCKOUT_MS of turning the encoder)
// is ignored until you release and tilt again. Set to 0 to go back to the
// old behavior.
#define DIR_REQUIRE_RELEASE_AFTER_GUARD 1


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

// Elevation ("height") mode (opened with a short press of Button B from the
// character detail screen - see main.cpp's g_elevEditing): encoder step size
// and bounds for the token's elevation, in the scene's own distance units
// (feet, meters, ...). Matches dnd5e's 5ft grid by default. Turning the
// encoder snaps the dialed-in value to the next multiple of ELEVATION_STEP in
// the direction of the turn (so a token at 7 goes to 10 or 5, not 12 or 2),
// the same way the circle radius moves in CIRCLE_RADIUS_STEP increments. Keep
// the bounds multiples of the step; they also keep the on-screen readout
// within four characters (e.g. "-995").
#define ELEVATION_STEP   5
#define ELEVATION_REFRESH_HOLD_MS 6000 // after sending an elevation, ignore the elevation in background refreshes for this long, so a
                                       // refresh that lands while Foundry is still animating the change can't show an intermediate
                                       // value - the target stays on screen, then the polled value takes over again
#define ELEVATION_MIN   -995
#define ELEVATION_MAX    995





// "Send Firmware" (unit-to-unit update, see main.cpp's sendFirmwareTo()).
// OTA_SEND_TIMEOUT_MS: how long a connect/socket write may stall before the
//   transfer is abandoned (the receiver stalls briefly while erasing flash
//   sectors, so don't set this too low).
// OTA_SEND_REPLY_MS: how long to wait for the receiver's "Update Success"
//   reply after the last byte is sent (it verifies the image first).
#define OTA_SEND_TIMEOUT_MS  8000
#define OTA_SEND_REPLY_MS    20000

// LittleFS path used to remember the relay address across reboots
#define RELAY_CONFIG_PATH "/relay.cfg"

// LittleFS path holding the saved WiFi networks (one "ssid<TAB>password"
// line each, oldest first) - see Storage::loadWifiNetworks() and main.cpp's
// connectWifi(). At boot every saved network is handed to
// ESP8266WiFiMulti, which connects to the strongest one currently in range;
// only if none connect does the WiFiManager config portal open.
#define WIFI_CONFIG_PATH "/wifi.cfg"
#define MAX_WIFI_NETWORKS 5            // oldest entry is dropped when a new one is added past this
#define WIFI_MULTI_TIMEOUT_MS 10000    // how long to wait for a saved network to connect before opening the portal

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
// Display.cpp draws these as pictograms rather than text wherever a hint
// contains a button label: a circle with an X inside for BUTTON_A_LABEL, a
// circle with an O inside for BUTTON_B_LABEL (long press adds a curve either
// side, double press shows the icon twice). The characters themselves still
// matter for the plain "Press X" style messages built in main.cpp.
#define BUTTON_A_LABEL (char)0x60  // backtick - patched to an X
#define BUTTON_B_LABEL (char)0x7E  // tilde - patched to a circle