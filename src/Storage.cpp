#include "Storage.h"
#include "config.h"
#include <LittleFS.h>

// The ESP8266 core has no NVS-backed Preferences API (that's ESP32-only),
// so this stores a single small text file on LittleFS instead: one line,
// "host,port".

namespace Storage {

void begin() {
  if (!LittleFS.begin()) {
    // First-ever boot on a totally fresh filesystem can fail to mount;
    // format once and retry. Harmless on subsequent boots since it only
    // runs when mounting actually failed.
    LittleFS.format();
    LittleFS.begin();
  }
}

bool loadRelayAddress(String &hostOut, uint16_t &portOut) {
  if (!LittleFS.exists(RELAY_CONFIG_PATH)) return false;

  File f = LittleFS.open(RELAY_CONFIG_PATH, "r");
  if (!f) return false;
  String line = f.readStringUntil('\n');
  f.close();
  line.trim();

  int comma = line.indexOf(',');
  if (comma <= 0) return false;

  String host = line.substring(0, comma);
  int port = line.substring(comma + 1).toInt();
  if (host.length() == 0 || port <= 0 || port > 65535) return false;

  hostOut = host;
  portOut = (uint16_t)port;
  return true;
}

void saveRelayAddress(const String &host, uint16_t port) {
  File f = LittleFS.open(RELAY_CONFIG_PATH, "w");
  if (!f) return;
  f.print(host);
  f.print(',');
  f.print(port);
  f.close();
}

void clearRelayAddress() {
  if (LittleFS.exists(RELAY_CONFIG_PATH)) {
    LittleFS.remove(RELAY_CONFIG_PATH);
  }
}

// Fixed-order CSV, one field per setting, in the same order as
// AppSettings' members: "rotation,gaugeFlat,serialDebug,relayTestMode,circleRadius,tableOrientation,moveButtonsEnabled".
// Same reasoning as the relay address above - this is deliberately a
// single small text file rather than anything more general, since the
// settings list is short and known at compile time.
static const int SETTINGS_FIELD_COUNT = 7;

bool loadSettings(AppSettings &out) {
  if (!LittleFS.exists(SETTINGS_CONFIG_PATH)) return false;

  File f = LittleFS.open(SETTINGS_CONFIG_PATH, "r");
  if (!f) return false;
  String line = f.readStringUntil('\n');
  f.close();
  line.trim();

  // Split into up to SETTINGS_FIELD_COUNT comma-separated fields, then
  // parse each one leniently rather than all-or-nothing: a missing/short
  // line (e.g. written by an older firmware with fewer settings than
  // AppSettings currently has) just leaves the trailing fields at
  // whatever `out` already had going in, instead of failing the whole
  // load. That way adding a new setting later doesn't invalidate anyone's
  // existing settings.cfg.
  String fields[SETTINGS_FIELD_COUNT];
  int fieldCount = 0;
  int start = 0;
  while (fieldCount < SETTINGS_FIELD_COUNT) {
    int comma = line.indexOf(',', start);
    if (comma < 0) {
      fields[fieldCount++] = line.substring(start);
      break;
    }
    fields[fieldCount++] = line.substring(start, comma);
    start = comma + 1;
  }

  if (fieldCount > 0) {
    fields[0].trim();
    if (fields[0].length() > 0) {
      int rot = fields[0].toInt();
      if (rot >= 0 && rot <= 3) {
        out.screenRotation = (uint8_t)rot;
      }
    }
  }
  if (fieldCount > 1) {
    fields[1].trim();
    if (fields[1].length() > 0) {
      out.gaugeStyleFlat = (fields[1].toInt() != 0);
    }
  }
  if (fieldCount > 2) {
    fields[2].trim();
    if (fields[2].length() > 0) {
      out.serialDebug = (fields[2].toInt() != 0);
    }
  }
  if (fieldCount > 3) {
    fields[3].trim();
    if (fields[3].length() > 0) {
      out.relayTestMode = (fields[3].toInt() != 0);
    }
  }
  if (fieldCount > 4) {
    fields[4].trim();
    if (fields[4].length() > 0) {
      float radius = fields[4].toFloat();
      if (radius > 0) {
        out.circleRadius = radius;
      }
    }
  }
  if (fieldCount > 5) {
    fields[5].trim();
    if (fields[5].length() > 0) {
      int orientation = fields[5].toInt();
      if (orientation >= 0 && orientation <= 3) {
        out.tableOrientation = (uint8_t)orientation;
      }
    }
  }
  if (fieldCount > 6) {
    fields[6].trim();
    if (fields[6].length() > 0) {
      out.moveButtonsEnabled = (fields[6].toInt() != 0);
    }
  }

  return true;
}

void saveSettings(const AppSettings &s) {
  File f = LittleFS.open(SETTINGS_CONFIG_PATH, "w");
  if (!f) return;
  f.print(s.screenRotation);
  f.print(',');
  f.print(s.gaugeStyleFlat ? 1 : 0);
  f.print(',');
  f.print(s.serialDebug ? 1 : 0);
  f.print(',');
  f.print(s.relayTestMode ? 1 : 0);
  f.print(',');
  f.print(s.circleRadius, 1);
  f.print(',');
  f.print(s.tableOrientation);
  f.print(',');
  f.print(s.moveButtonsEnabled ? 1 : 0);
  f.close();
}

} // namespace Storage