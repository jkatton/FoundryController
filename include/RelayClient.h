#pragma once
#include <Arduino.h>
#include "Types.h"

// Thin wrapper around the FoundryVTT REST API Relay's HTTP surface.
// Endpoint paths and JSON field names are all centralized here -
// if your relay version differs, this is the only file you should
// need to touch.
namespace RelayClient {

  // Runtime toggle for the in-RAM mock party vs the real relay's HTTP
  // surface ("Local Relay Test Mode" on the settings screen) - defaults to
  // RELAY_TEST_MODE from config.h. Changing it takes effect the next time
  // you (re)connect (begin()+connect()), not mid-connection.
  void setTestMode(bool enabled);
  bool testMode();

  void begin(const String &host, uint16_t port);

  // Quick reachability + "is a Foundry world actually connected" check.

  bool connect();

  // true once connect() has succeeded and a clientId is cached.
  bool isConnected();

  // Fetches all player-character actors (type == ACTOR_TYPE_FILTER).
  // Returns true on success and fills `out`.
  bool fetchCharacters(std::vector<Character> &out);

  // Pushes a new hp value (and temp hp) to the server for the given actor.
  bool updateCharacterHp(const String &uuid, int newHp, int newTempHp);

  String lastError();


}
