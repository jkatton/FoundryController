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
};

// HP adjustment mode within the detail screen
enum class HpAddTarget { NORMAL, TEMP };

// Button press classification
enum class ButtonEvent {
  NONE,
  SHORT_PRESS,
  LONG_PRESS,
  DOUBLE_PRESS
};
