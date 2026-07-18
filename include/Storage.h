#pragma once
#include <Arduino.h>
#include "Types.h"

namespace Storage {
  void begin();

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
