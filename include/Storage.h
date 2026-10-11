#pragma once
#include <Arduino.h>
#include "Types.h"

struct WifiCred {
  String ssid;
  String pass;
};

namespace Storage {
  void begin();

  // Saved WiFi networks (up to MAX_WIFI_NETWORKS, oldest first). load
  // returns true if at least one was read. add moves an already-known SSID
  // to the newest slot (updating its password) instead of duplicating it,
  // and drops the oldest entry if the list would exceed the cap.
  bool loadWifiNetworks(std::vector<WifiCred> &out);
  void addWifiNetwork(const String &ssid, const String &pass);
  void removeWifiNetwork(const String &ssid);
  void clearWifiNetworks();

  // True once wifi.cfg has ever been written (even if it's now empty after
  // deleting every network). main.cpp's one-time migration of the SDK's
  // remembered network only runs while this is still false, so deleting
  // the last saved network doesn't get silently undone on the next boot.
  bool wifiConfigExists();

  // Returns true if a previously saved address exists.
  bool loadRelayAddress(String &hostOut, uint16_t &portOut);

  void saveRelayAddress(const String &host, uint16_t port);
  void clearRelayAddress();

  // Loads settings.cfg into `out`. Only overwrites the fields it can
  // actually parse - any field missing from the file (including the whole
  // file not existing yet, e.g. first boot or an older firmware that saved
  // fewer settings) is left at whatever `out` already held going in, so
  // callers should pre-populate `out` with AppSettings' defaults first.
  // Returns true if the file existed and was read at all.
  bool loadSettings(AppSettings &out);

  void saveSettings(const AppSettings &s);
}