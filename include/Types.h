#pragma once
#include <Arduino.h>
#include <vector>
#include "config.h"

// One player character as pulled from the relay.
struct Character {
  String uuid;       // Foundry document UUID, e.g. "Actor.abc123"
  String name;
  int hp = 0;         // system.attributes.hp.value
  int maxHp = 1;       // system.attributes.hp.max
  int tempHp = 0;      // system.attributes.hp.temp
};

// Application state machine
enum class AppState {
  BOOT,
  WIFI_CONNECTING,
  RELAY_CONNECTING,        // trying saved address
  IP_ENTRY,                 // user is keying in an address with the encoder
  RELAY_TESTING_ENTRY,      // trying the address the user just entered
  LOADING_CHARACTERS,
  CHARACTER_LIST,
  CHARACTER_DETAIL,
  SETTINGS,                 // carousel of user-editable settings, reached by
                             // long-pressing button B from IP_ENTRY
  ERROR_SCREEN
};

// User-editable settings, persisted via Storage::loadSettings/saveSettings
// the same way the relay address is - a small fixed-order line on
// LittleFS (see Storage.cpp). To add a new setting: add a field here, add
// a SettingDescriptor for it in main.cpp's SETTINGS_TABLE, and extend the
// load/save parsing in Storage.cpp to read/write the new field.
//
// serialDebug and relayTestMode default from config.h's INPUT_DEBUG/
// RELAY_TEST_MODE macros - those used to be compile-time-only #if guards;
// they're now runtime flags (see InputManager::setDebugEnabled() and
// RelayClient::setTestMode()) so they can live on the settings screen, but
// still start out matching whatever config.h says until a settings.cfg
// says otherwise.
struct AppSettings {
  uint8_t screenRotation = 2;                     // TFT_eSPI rotation, 0-3; 2 matches the display's previous hardcoded default
  bool gaugeStyleFlat = true;                     // HP ring style: flat quantized bands (true) vs smooth gradient (false)
  bool serialDebug = false;          // verbose encoder/button/error prints to Serial
  bool relayTestMode = false;    // drive the UI from an in-RAM mock party instead of the real relay's HTTP surface
  float circleRadius = CIRCLE_RADIUS_DEFAULT; // last radius used for the circle (main.cpp's g_circleRadius) - persisted so
                                              // it carries over between reboots, but not part of the settings carousel
                                              // (SETTINGS_TABLE) since it's set live via the encoder in radius-editing itself
};

// HP adjustment mode within the detail screen
enum class HpAddTarget { NORMAL, TEMP };

// Movement-mode display state for Display::showCharacterDetail(). Passed
// in place of/alongside the pending-HP-delta info when the character
// detail screen has been switched into "move mode" (a joystick direction
// was pressed) - see main.cpp's g_moveMode. When active is false, the
// screen shows its normal pending-HP-delta row instead and every other
// field here is ignored.
struct MovementIndicator {
  bool active = false;    // true while the detail screen is in move mode
  int dCol = 0;            // accumulated columns nudged (+right/-left) since entering move mode
  int dRow = 0;            // accumulated rows nudged (+down/-up)
  float distance = 0;      // distance to the accumulated target, as last reported by the relay
  String units;            // scene distance units for `distance`, e.g. "ft" (empty if not yet known)
  bool sending = false;    // a previewMove()/confirmMove() call is in flight
};

// Circle-mode display state for Display::showCharacterDetail(). A short
// press of Button B opens radius-editing, handing the encoder over to
// dialing the radius (see main.cpp's g_circleEditing) without disturbing
// whichever of HP-edit or move is currently the shared sub-display. Once
// radius-editing is open, the two presses of Button B diverge: a second
// short press removes whatever circle is currently drawn but stays in
// radius-editing with the dialed-in radius unchanged, while a long press
// instead exits radius-editing (resuming whichever of HP-edit/move was
// showing) but leaves the circle exactly as drawn (see `drawn` below).
struct CircleIndicator {
  bool active = false;    // true while the encoder is actively dialing the radius (opened by a short press of Button B, closed by a long press)
  bool drawn = false;     // true whenever a circle currently exists on the map for this character, whether or not `active` is - drives the small radius readout near the HP display (see Display.cpp), independent of whichever of HP/move is the current big sub-display
  float radius = 0;        // current radius, in the scene's own distance units
  String units;            // best-effort guess at those units (empty if not yet known - see main.cpp)
  bool sending = false;    // a previewCircle() call is in flight
};

// Button press classification
enum class ButtonEvent {
  NONE,
  SHORT_PRESS,
  LONG_PRESS,
  DOUBLE_PRESS
};