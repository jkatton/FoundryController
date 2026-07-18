#include <Arduino.h>
#include "RelayClient.h"
#include "config.h"
#include "Display.h"

#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <ArduinoJson.h>

// -------------------------------------------------------------------------
// NOTE ON API SHAPE
// -------------------------------------------------------------------------
// This targets the self-hosted "hp-bridge" relay (see hp-bridge-module/ in
// this project), not a generic Foundry REST API. Characters are opted in
// one at a time from a checkbox the hp-bridge Foundry module adds to each
// character sheet ("Send to HP Monitor") - only those characters are ever
// visible here. The HTTP surface is deliberately just two endpoints:
//
//   GET  /characters         -> every opted-in character's full data:
//                                {"characters":[{"uuid","name","hp","maxHp","tempHp"}, ...]}
//   POST /characters/update  -> body {"uuid":"...","hp":N,"tempHp":N}
//                                updates one of those characters
//   GET  /healthz            -> {"ok":true,"foundryConnected":bool}, used
//                                by connect() to confirm both the relay and
//                                a Foundry world are reachable
//
// There is no authentication on requests to the relay - it's assumed to be
// running on a trusted local network alongside the Foundry world it talks
// to.
//
// NOTE ON STORAGE: nothing in this file touches LittleFS. Storage.cpp's
// relay host/port persistence is unrelated and untouched.
// -------------------------------------------------------------------------

// -------------------------------------------------------------------------
// TEST MODE
// -------------------------------------------------------------------------
// Runtime-toggleable via setTestMode()/testMode() - and the "Local Relay
// Test Mode" entry on the settings screen - rather than the compile-time
// #if this used to be. Defaults to RELAY_TEST_MODE from config.h. When
// enabled, connect()/fetchCharacters()/updateCharacterHp() skip the
// relay's HTTP surface entirely and operate on a small predefined party
// kept in RAM instead: connect() "succeeds" after a short simulated
// delay, so showRelayConnecting() is still visible for a beat, and every
// fetch/update call operates on the in-memory roster in mockParty()
// below. Edit that list to whatever party/HP spread is useful to test
// with. Toggling it takes effect the next time you (re)connect - i.e. the
// next long-press on the IP entry screen - not mid-connection. main.cpp
// needs no changes either way - this only swaps out what's behind the
// RelayClient:: functions it already calls.
// -------------------------------------------------------------------------
#ifndef RELAY_TEST_MODE
#define RELAY_TEST_MODE 0
#endif

namespace RelayClient
{

  static bool s_testMode = (RELAY_TEST_MODE != 0);

  static String s_host;
  static uint16_t s_port = RELAY_DEFAULT_PORT;
  static String s_lastError;
  static bool s_connected = false;

  void setTestMode(bool enabled) { s_testMode = enabled; }
  bool testMode() { return s_testMode; }

  // -----------------------------------------------------------------------
  // Mock party (test mode)
  // -----------------------------------------------------------------------
  static void addMockCharacter(std::vector<Character> &v, const char *uuid, const char *name,
                               int hp, int maxHp, int tempHp)
  {
    Character c;
    c.uuid = uuid;
    c.name = name;
    c.hp = hp;
    c.maxHp = maxHp;
    c.tempHp = tempHp;
    v.push_back(c);
  }

  // HP values below are picked to exercise all three ring colors
  // (green/yellow/red) and the temp-HP row on the character detail screen.
  static std::vector<Character> &mockParty()
  {
    static std::vector<Character> party;
    static bool initialized = false;
    if (!initialized)
    {
      addMockCharacter(party, "mock-uuid-1", "Thalric Ironvein", 42, 54, 5);         // green, temp HP
      addMockCharacter(party, "mock-uuid-2", "Sable Nightshade", 18, 36, 0);         // yellow
      addMockCharacter(party, "mock-uuid-3", "Boulder", 60, 60, 0);                  // green, full/max HP
      addMockCharacter(party, "mock-uuid-4", "Pip Quickfingers", 3, 22, 2);          // red, temp HP
      addMockCharacter(party, "mock-uuid-5", "Longname von Blahdeeblah", 60, 65, 0); // red, temp HP
      initialized = true;
    }
    return party;
  }

  // -----------------------------------------------------------------------
  // Real relay (HTTP) helpers
  // -----------------------------------------------------------------------
  static String baseUrl()
  {
    return "http://" + s_host + ":" + String(s_port);
  }

  static void applyCommonHeaders(HTTPClient &http)
  {
    http.setTimeout(RELAY_HTTP_TIMEOUT_MS);
  }

  // Parses one entry of the /characters response array into a Character.
  static bool parseCharacterEntry(JsonObject obj, Character &out)
  {
    if (obj["uuid"].isNull() || obj["name"].isNull())
      return false;

    out.uuid = obj["uuid"].as<String>();
    out.name = obj["name"].as<String>();
    out.hp = obj["hp"] | 0;
    out.maxHp = obj["maxHp"] | 1;
    out.tempHp = obj["tempHp"] | 0;
    return true;
  }

  // -----------------------------------------------------------------------
  // Public API - each function branches on s_testMode up front
  // -----------------------------------------------------------------------

  void begin(const String &host, uint16_t port)
  {
    s_host = host;
    s_port = port;
    s_connected = false;
    s_lastError = "";
  }

  String lastError() { return s_lastError; }
  bool isConnected() { return s_connected; }

  bool connect()
  {
    if (s_testMode)
    {
      delay(400); // stand-in for real connect latency, so showRelayConnecting() is still visible for a beat
      yield();
      s_connected = true;
      return true;
    }

    s_connected = false;

    if (s_host.length() == 0)
    {
      s_lastError = "no host set";
      return false;
    }

    HTTPClient http;
    WiFiClient client;
    String url = baseUrl() + "/healthz";
    if (!http.begin(client, url))
    {
      s_lastError = "http.begin failed";
      return false;
    }
    applyCommonHeaders(http);
    http.setTimeout(RELAY_TEST_TIMEOUT_MS);

    int code = http.GET();
    if (code != HTTP_CODE_OK)
    {
      s_lastError = "GET /healthz -> " + String(code);
      http.end();
      return false;
    }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, http.getStream());
    http.end();
    if (err)
    {
      s_lastError = "healthz JSON parse: " + String(err.c_str());
      return false;
    }

    bool foundryConnected = doc["foundryConnected"] | false;
    if (!foundryConnected)
    {
      s_lastError = "relay reachable but no Foundry world connected";
      return false;
    }

    s_connected = true;
    return true;
  }

  // The one and only "give me everyone" call. Every character checked
  // "Send to HP Monitor" in Foundry comes back in a single response, hp
  // values included - no follow-up per-character fetch needed.
  bool fetchCharacters(std::vector<Character> &out)
  {
    if (s_testMode)
    {
      out = mockParty();
      return true;
    }

    out.clear();
    if (!s_connected)
    {
      s_lastError = "not connected";
      return false;
    }

    HTTPClient http;
    WiFiClient client;
    String url = baseUrl() + "/characters";
    if (!http.begin(client, url))
    {
      s_lastError = "http.begin failed";
      return false;
    }
    applyCommonHeaders(http);

    int code = http.GET();
    if (code != HTTP_CODE_OK)
    {
      s_lastError = "GET /characters -> " + String(code);
      http.end();
      return false;
    }

    // JsonDocument grows on the heap as needed, so no fixed capacity to
    // size for a modest party (a handful of characters at a few dozen
    // bytes each) like the old DynamicJsonDocument required.
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, http.getStream());
    http.end();
    if (err)
    {
      s_lastError = "characters JSON parse: " + String(err.c_str());
      return false;
    }

    JsonArray arr = doc["characters"].as<JsonArray>();
    if (arr.isNull())
    {
      s_lastError = "unexpected /characters response shape";
      return false;
    }

    for (JsonObject obj : arr)
    {
      Character c;
      if (parseCharacterEntry(obj, c))
        out.push_back(c);
    }

    return true;
  }

  bool updateCharacterHp(const String &uuid, int newHp, int newTempHp)
  {
    if (s_testMode)
    {
      for (auto &c : mockParty())
      {
        if (c.uuid == uuid)
        {
          c.hp = newHp;
          c.tempHp = newTempHp;
          return true;
        }
      }
      s_lastError = "mock: no character with uuid " + uuid;
      return false;
    }

    if (!s_connected)
    {
      s_lastError = "not connected";
      return false;
    }

    HTTPClient http;
    WiFiClient client;
    String url = baseUrl() + "/characters/update";
    if (!http.begin(client, url))
    {
      s_lastError = "http.begin failed";
      return false;
    }
    applyCommonHeaders(http);
    http.addHeader("Content-Type", "application/json");

    JsonDocument doc;
    doc["uuid"] = uuid;
    doc["hp"] = newHp;
    doc["tempHp"] = newTempHp;

    String body;
    serializeJson(doc, body);

    int code = http.POST(body);

    if (code != HTTP_CODE_OK && code != 201 && code != 204)
    {
      // Try to surface the relay's own error message if it sent JSON back.
      JsonDocument errDoc;
      if (deserializeJson(errDoc, http.getStream()) == DeserializationError::Ok && !errDoc["error"].isNull())
      {
        s_lastError = "POST /characters/update -> " + String(code) + ": " + errDoc["error"].as<String>();
      }
      else
      {
        s_lastError = "POST /characters/update -> " + String(code);
      }
      http.end();
      return false;
    }
    http.end();
    return true;
  }

} // namespace RelayClient
