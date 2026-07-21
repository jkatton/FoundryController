# Foundry HP Monitor (Wemos D1 Mini / ESP8266 + rotary encoder + round LCD)

A standalone hardware "party HP tracker" for FoundryVTT. It connects to your
WiFi (via WiFiManager's captive-portal setup, no hardcoded SSID/password),
then talks to a running custom npm relay that talks to a Foundry module
instance to list player characters and read/write their hit points. All
control is via one rotary encoder (with an integrated push button, "Button A")
plus one extra standalone push button ("Button B").

## Hardware

- **Wemos D1 Mini** (ESP8266) board
- Round 240x240 SPI LCD, **GC9A01 driver** (the common 1.28" round module)
- Rotary encoder (KY-040 style, quadrature + push button + 4 way joystick)
- One extra momentary push button

TRACE MUST BE CUT ON D1 MINI TO ALLOW THE USE OF d0 pin for encoder (see photo).

ONCE TRACE IS CUT, IN ORDER TO ENTER PROGRAMMING MODE PIN 0 (D3) MUST BE 
SHORTED TO GND BEFORE RST BUTTON IS PRESSED


### Wiring (matches `include/config.h` / `platformio.ini` defaults)

The D1 Mini only exposes 9 usable digital GPIOs (D0-D8), and this project
needs exactly that many — so the display backlight is wired straight to
3V3 instead of through a GPIO; there's no pin to spare for it.

// Rotary encoder (RKJXT1F42001)
#define PIN_ENCODER_CLK     5   // D1
#define PIN_ENCODER_DT      4   // D2

// I2C bus to the PCF8575 expander

#define PIN_I2C_SDA         0   // D3 - see constraint above re: USB auto-reset contention
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
#define PIN_TFT_RST         -1   // not used, doftware reset instead
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
#define EXP_PIN_JOY_UP         2   // P1
#define EXP_PIN_JOY_DOWN       1   // P2
#define EXP_PIN_JOY_LEFT       4   // P3
#define EXP_PIN_JOY_RIGHT      3   // P4
#define EXP_PIN_BUTTON2        5   // P5 - standalone "Button B"

Both buttons are wired active-low to GND with the internal `INPUT_PULLUP`
resistors enabled in software — no external resistors needed. There's no
MISO wire: the GC9A01 is write-only for our purposes.

A couple of ESP8266-specific quirks this mapping works around:

- **SPI pins aren't freely reassignable like on an ESP32.** Hardware SPI
  only runs on GPIO13 (MOSI) and GPIO14 (SCLK) — those two must stay put
  even if you move everything else. `TFT_CS`/`TFT_DC`/`TFT_RST` are plain
  `digitalWrite` pins and can go anywhere.
- **GPIO16 (D0) has no interrupt support and no usable internal pull-up**,
  so it can't drive the encoder or a pulled-up button. It's used for
  `TFT_RST` here instead, which is just a plain output.
- **D3 (GPIO0) and D4 (GPIO2) are boot-strapping pins** — they need to read
  HIGH at power-on/reset for the board to boot normally. Both are used for
  buttons here (idle-high via `INPUT_PULLUP`, only pulled low when
  pressed) — this is the same pattern the D1 Mini's own onboard "FLASH"
  button uses on D3. The only real-world consequence is: don't power the
  board on, or hit reset, while physically holding one of the buttons
  down, or it may drop into the bootloader instead of booting normally.
  D4 also drives the onboard blue LED (active-low), so it'll flicker
  faintly whenever Button B is held — cosmetic only.

Change any of these in `include/config.h`, and mirror the TFT pins in
`platformio.ini`'s `build_flags` (they're duplicated there because
TFT_eSPI needs them as compile-time defines).

## Building

This is a **PlatformIO** project (recommended, since it configures TFT_eSPI
cleanly via build flags rather than requiring you to hand-edit files inside
the library). Install the [PlatformIO extension for VS Code](https://platformio.org/install/ide?install=vscode)
or the CLI, open this folder, and run:

```
pio run -t upload
pio device monitor
```

Libraries (`WiFiManager`, `TFT_eSPI`, `ArduinoJson`) are pulled automatically
via `lib_deps` in `platformio.ini`. The D1 Mini has an onboard CH340 or
CP2102 USB-serial chip (not native USB), so flashing/monitoring is the
usual straightforward `/dev/ttyUSB0`-style serial port — no special reset
sequence needed.

If you'd rather use the Arduino IDE: install those three libraries via the
Library Manager, then you'll need to manually configure `TFT_eSPI`'s
`User_Setup.h` (in the library folder) with the same GC9A01 driver and pin
settings shown in `platformio.ini`, since the Arduino IDE doesn't support
per-project build flags the way PlatformIO does. You'll also want to select
a **LittleFS** flash-size/filesystem option under Tools before your first
upload (see the storage note below).

## First run / usage

1. On first boot (or if the saved relay address stops working), the device
   starts a WiFi config portal — connect to the **FoundryHPMonitor-Setup**
   AP from your phone/laptop and pick your WiFi network. This uses
   WiFiManager, so no credentials are ever hardcoded.
2. It then tries the last relay address you entered (saved in flash). If
   that fails (or it's the first boot), you'll enter the relay server's
   IP address and port with the encoder: turn to change the highlighted
   octet/port, short-press **Button A** to move to the next field, long-press
   **Button A** to connect.
3. Once connected, it pulls all player-character actors and shows a
   scrollable list — turn the encoder to scroll, short-press **A** to open a
   character.
4. On the character screen: the ring shows current HP as a percentage of max
   (green/yellow/red). Turning the encoder changes the value of an HP 
   adjustment (positive for healing, negative for damage). Short-press **A**
   sends the adjustment to the game. Short-press **B** changes the effect of 
   positive adjustment between regular and temp hp (negative adjustment is
   always the same: reduce temp hp first, then regular hp). Long-press **A** 
   discards changes and returns to the list.
5. Both the list and the open character screen poll the
   relay automatically every few seconds (`CHARACTER_POLL_MS` in
   `config.h`) so the display stays live if HP changes from inside Foundry
   itself — this pauses for a given character while you have an uncommitted
   edit in progress, so it won't yank the dial out from under you mid-turn.
6. Long-press **B** on the list screen re-opens IP entry if you need to
   point the device at a different relay/session.



- HP updates are sent only when you press **A** — turning the encoder never
  writes to Foundry by itself, only to the local display.
- The relay address (not WiFi credentials — those are owned by
  WiFiManager/the ESP8266 WiFi stack) is remembered across reboots as a
  small text file on **LittleFS**, since the ESP8266 core doesn't have the
  ESP32's NVS-backed `Preferences` API. `Storage::begin()` mounts (and, on
  a completely fresh board, formats) the filesystem automatically — no
  manual setup needed, though the Arduino IDE does need a LittleFS option
  selected under Tools if you're not using PlatformIO.
