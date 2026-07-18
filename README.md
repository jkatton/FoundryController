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
- Rotary encoder (KY-040 style, quadrature + push button)
- One extra momentary push button

### Wiring (matches `include/config.h` / `platformio.ini` defaults)

The D1 Mini only exposes 9 usable digital GPIOs (D0-D8), and this project
needs exactly that many — so the display backlight is wired straight to
3V3 instead of through a GPIO; there's no pin to spare for it.

| Signal              | D1 Mini label | GPIO |
|---------------------|---------------|------|
| Encoder CLK          | D1 | GPIO5 |
| Encoder DT            | D2 | GPIO4 |
| Encoder SW (Button A) | D3 | GPIO0 |
| Button B               | D4 | GPIO2 |
| TFT MOSI                | D7 | GPIO13 |
| TFT SCLK                 | D5 | GPIO14 |
| TFT CS                     | D8 | GPIO15 |
| TFT DC                       | D6 | GPIO12 |
| TFT RST                        | D0 | GPIO16 |
| TFT Backlight                    | — | wire directly to **3V3** |

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
