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
// AppSettings' members: "rotation,gaugeStyleFlat,serialDebug,relayTestMode,circleRadius,tableOrientation,moveButtonsEnabled,swapButtons,circleEnabled".
// Same reasoning as the relay address above - this is deliberately a
// single small text file rather than anything more general, since the
// settings list is short and known at compile time.
static const int SETTINGS_FIELD_COUNT = 9;

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
  if (fieldCount > 7) {
    fields[7].trim();
    if (fields[7].length() > 0) {
      out.swapButtons = (fields[7].toInt() != 0);
    }
  }
  if (fieldCount > 8) {
    fields[8].trim();
    if (fields[8].length() > 0) {
      out.circleEnabled = (fields[8].toInt() != 0);
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
  f.print(',');
  f.print(s.swapButtons ? 1 : 0);
  f.print(',');
  f.print(s.circleEnabled ? 1 : 0);
  f.close();
}

// ---------------------------------------------------------------------
// Saved WiFi networks: one "ssid<TAB>password" line per network, oldest
// first. Tab is the separator (rather than a comma like the other config
// files) since commas are common in SSIDs/passwords; entries containing a
// tab or newline are simply refused on save.
// ---------------------------------------------------------------------
bool loadWifiNetworks(std::vector<WifiCred> &out) {
  out.clear();
  if (!LittleFS.exists(WIFI_CONFIG_PATH)) return false;

  File f = LittleFS.open(WIFI_CONFIG_PATH, "r");
  if (!f) return false;
  while (f.available() && (int)out.size() < MAX_WIFI_NETWORKS) {
    String line = f.readStringUntil('\n');
    line.replace("\r", "");
    if (line.length() == 0) continue;
    int tab = line.indexOf('\t');
    WifiCred c;
    c.ssid = (tab < 0) ? line : line.substring(0, tab);
    c.pass = (tab < 0) ? String("") : line.substring(tab + 1);
    if (c.ssid.length() == 0) continue;
    out.push_back(c);
  }
  f.close();
  return !out.empty();
}

static void writeWifiNetworks(const std::vector<WifiCred> &nets) {
  File f = LittleFS.open(WIFI_CONFIG_PATH, "w");
  if (!f) return;
  for (size_t i = 0; i < nets.size(); i++) {
    f.print(nets[i].ssid);
    f.print('\t');
    f.print(nets[i].pass);
    f.print('\n');
  }
  f.close();
}

static bool wifiFieldOk(const String &s) {
  return s.indexOf('\t') < 0 && s.indexOf('\n') < 0 && s.indexOf('\r') < 0;
}

void addWifiNetwork(const String &ssid, const String &pass) {
  if (ssid.length() == 0 || !wifiFieldOk(ssid) || !wifiFieldOk(pass)) return;

  std::vector<WifiCred> nets;
  loadWifiNetworks(nets);
  for (size_t i = 0; i < nets.size(); i++) {
    if (nets[i].ssid == ssid) {
      nets.erase(nets.begin() + i);
      break;
    }
  }
  WifiCred c;
  c.ssid = ssid;
  c.pass = pass;
  nets.push_back(c);
  while ((int)nets.size() > MAX_WIFI_NETWORKS) nets.erase(nets.begin());
  writeWifiNetworks(nets);
}

void removeWifiNetwork(const String &ssid) {
  std::vector<WifiCred> nets;
  loadWifiNetworks(nets);
  for (size_t i = 0; i < nets.size(); i++) {
    if (nets[i].ssid == ssid) {
      nets.erase(nets.begin() + i);
      break;
    }
  }
  writeWifiNetworks(nets); // writes an empty file if that was the last one - see wifiConfigExists()
}

void clearWifiNetworks() {
  std::vector<WifiCred> none;
  writeWifiNetworks(none);
}

bool wifiConfigExists() {
  return LittleFS.exists(WIFI_CONFIG_PATH);
}

} // namespace Storage