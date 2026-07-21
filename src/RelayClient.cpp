#include <Arduino.h>
#include "RelayClient.h"
#include "config.h"
#include "Display.h"

#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <ArduinoJson.h>
#include <map>

// -------------------------------------------------------------------------
// NOTE ON API SHAPE
// -------------------------------------------------------------------------
// This targets the self-hosted "foundry-controller" relay (see
// foundry-controller-module/ in this project), not a generic Foundry REST
// API. Characters are opted in one at a time from a checkbox the
// foundry-controller Foundry module adds to each character sheet ("Send to
// HP Monitor") - only those characters are ever visible here. The HTTP
// surface:
//
//   GET  /characters          -> every opted-in character's full data:
//                                 {"characters":[{"uuid","name","hp","maxHp","tempHp"}, ...]}
//   POST /characters/update   -> body {"uuid":"...","hp":N,"tempHp":N}
//                                 updates one of those characters
//   POST /tokens/move         -> body {"uuid":"...","col":N,"row":N,"timeoutMs":N}
//                                 or  {"uuid":"...","dCol":N,"dRow":N,"timeoutMs":N}
//                                 sets (but does not yet commit) a target
//                                 for a character's token - either an
//                                 absolute grid cell, or an offset from its
//                                 actual current position; the response
//                                 reports the distance to it
//   POST /tokens/move/confirm -> body {"uuid":"..."}
//                                 commits whichever target the last
//                                 /tokens/move call set for that uuid
//   POST /tokens/move/cancel  -> body {"uuid":"..."}
//                                 discards the pending target instead
//   POST /tokens/circle       -> body {"uuid":"...","radius":N,"color":"#rrggbb","durationMs":N}
//                                 drops a real, persistent circular map
//                                 template centered on a character's
//                                 token - implicitly clears any live
//                                 preview from /tokens/circle/preview
//   POST /tokens/circle/preview -> body {"uuid":"...","radius":N,"color":"#rrggbb","timeoutMs":N}
//                                 live, local-only preview of a circle's
//                                 radius - safe to call repeatedly as a
//                                 radius is dialed in
//   POST /tokens/circle/cancel  -> body {"uuid":"..."}
//                                 discards the live preview instead of
//                                 committing it
//   GET  /healthz              -> {"ok":true,"foundryConnected":bool}, used
//                                 by connect() to confirm both the relay and
//                                 a Foundry world are reachable
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
// enabled, connect()/fetchCharacters()/updateCharacterHp()/previewMove()/
// previewMoveRelative()/confirmMove()/cancelMove()/createCircle()/
// previewCircle()/cancelCirclePreview() skip the relay's HTTP surface
// entirely and operate on a small predefined party kept in RAM instead:
// connect() "succeeds" after a short simulated delay, so
// showRelayConnecting() is still visible for a beat, and every
// fetch/update call operates on the in-memory roster in mockParty()
// below (token positions for move previews live alongside it in
// mockTokenStates()). Edit that list to whatever party/HP spread is
// useful to test with. Toggling it takes effect the next time you
// (re)connect - i.e. the next long-press on the IP entry screen - not
// mid-connection. main.cpp needs no changes either way - this only swaps
// out what's behind the RelayClient:: functions it already calls.
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

  // Tracks each mock character's simulated token position, plus at most
  // one pending (unconfirmed) move target, so previewMove()/confirmMove()/
  // cancelMove() have something realistic to operate on in test mode.
  // Positions start at (0, 0) the first time a given uuid is touched.
  struct MockTokenState
  {
    int col = 0;
    int row = 0;
    bool hasPending = false;
    int pendingCol = 0;
    int pendingRow = 0;
  };

  static std::map<String, MockTokenState> &mockTokenStates()
  {
    static std::map<String, MockTokenState> states;
    return states;
  }

  // Assumes 5ft-per-cell for the simulated distance readout, matching the
  // most common dnd5e default - test mode doesn't model actual scene grid
  // configuration.
  static float mockCellDistanceUnit() { return 5.0f; }

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

  // Shared by updateCharacterHp()/previewMove()/confirmMove()/cancelMove()/
  // createCircle(): POSTs a JSON body to `path` on the relay and parses
  // the JSON response into `responseDoc`. On any failure (not connected,
  // transport error, non-2xx status, malformed response body) fills
  // `errOut` and returns false, pulling the relay's own {"error":"..."}
  // message out of the body when present.
  static bool doPost(const String &path, const JsonDocument &bodyDoc, JsonDocument &responseDoc, String &errOut)
  {
    if (!s_connected)
    {
      errOut = "not connected";
      return false;
    }

    HTTPClient http;
    WiFiClient client;
    String url = baseUrl() + path;
    if (!http.begin(client, url))
    {
      errOut = "http.begin failed";
      return false;
    }
    applyCommonHeaders(http);
    http.addHeader("Content-Type", "application/json");

    String body;
    serializeJson(bodyDoc, body);
    int code = http.POST(body);

    if (code != HTTP_CODE_OK && code != 201 && code != 204)
    {
      JsonDocument errDoc;
      if (deserializeJson(errDoc, http.getStream()) == DeserializationError::Ok && !errDoc["error"].isNull())
      {
        errOut = "POST " + path + " -> " + String(code) + ": " + errDoc["error"].as<String>();
      }
      else
      {
        errOut = "POST " + path + " -> " + String(code);
      }
      http.end();
      return false;
    }

    DeserializationError err = deserializeJson(responseDoc, http.getStream());
    http.end();
    if (err)
    {
      errOut = path + " JSON parse: " + String(err.c_str());
      return false;
    }
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

    JsonDocument reqDoc;
    reqDoc["uuid"] = uuid;
    reqDoc["hp"] = newHp;
    reqDoc["tempHp"] = newTempHp;

    JsonDocument respDoc;
    String errOut;
    if (!doPost("/characters/update", reqDoc, respDoc, errOut))
    {
      s_lastError = errOut;
      return false;
    }

    return true;
  }

  bool previewMove(const String &uuid, int col, int row, MovePreviewResult &out, uint32_t timeoutMs)
  {
    if (s_testMode)
    {
      MockTokenState &state = mockTokenStates()[uuid]; // default-constructs at (0,0) if new
      state.pendingCol = col;
      state.pendingRow = row;
      state.hasPending = true;

      float dx = float(col - state.col);
      float dy = float(row - state.row);
      out.col = col;
      out.row = row;
      out.distance = sqrtf(dx * dx + dy * dy) * mockCellDistanceUnit();
      out.units = "ft";
      return true;
    }

    if (!s_connected)
    {
      s_lastError = "not connected";
      return false;
    }

    JsonDocument reqDoc;
    reqDoc["uuid"] = uuid;
    reqDoc["col"] = col;
    reqDoc["row"] = row;
    if (timeoutMs > 0)
      reqDoc["timeoutMs"] = timeoutMs;

    JsonDocument respDoc;
    String errOut;
    if (!doPost("/tokens/move", reqDoc, respDoc, errOut))
    {
      s_lastError = errOut;
      return false;
    }

    JsonObject preview = respDoc["preview"].as<JsonObject>();
    if (preview.isNull())
    {
      s_lastError = "unexpected /tokens/move response shape";
      return false;
    }

    out.col = preview["col"] | col;
    out.row = preview["row"] | row;
    out.distance = preview["distance"] | 0.0f;
    out.units = preview["units"] | "";
    return true;
  }

  bool previewMoveRelative(const String &uuid, int dCol, int dRow, MovePreviewResult &out, uint32_t timeoutMs)
  {
    if (s_testMode)
    {
      MockTokenState &state = mockTokenStates()[uuid]; // default-constructs at (0,0) if new
      // Always relative to the confirmed position (state.col/row), never
      // to an earlier pending target - matches the relay's own semantics,
      // so a caller can resend the running total on every nudge.
      state.pendingCol = state.col + dCol;
      state.pendingRow = state.row + dRow;
      state.hasPending = true;

      out.col = state.pendingCol;
      out.row = state.pendingRow;
      out.distance = sqrtf(float(dCol * dCol + dRow * dRow)) * mockCellDistanceUnit();
      out.units = "ft";
      return true;
    }

    if (!s_connected)
    {
      s_lastError = "not connected";
      return false;
    }

    JsonDocument reqDoc;
    reqDoc["uuid"] = uuid;
    reqDoc["dCol"] = dCol;
    reqDoc["dRow"] = dRow;
    if (timeoutMs > 0)
      reqDoc["timeoutMs"] = timeoutMs;

    JsonDocument respDoc;
    String errOut;
    if (!doPost("/tokens/move", reqDoc, respDoc, errOut))
    {
      s_lastError = errOut;
      return false;
    }

    JsonObject preview = respDoc["preview"].as<JsonObject>();
    if (preview.isNull())
    {
      s_lastError = "unexpected /tokens/move response shape";
      return false;
    }

    out.col = preview["col"] | 0;
    out.row = preview["row"] | 0;
    out.distance = preview["distance"] | 0.0f;
    out.units = preview["units"] | "";
    return true;
  }

  bool confirmMove(const String &uuid, MoveResult &out)
  {
    if (s_testMode)
    {
      auto it = mockTokenStates().find(uuid);
      if (it == mockTokenStates().end() || !it->second.hasPending)
      {
        s_lastError = "mock: no pending move target for uuid " + uuid;
        return false;
      }
      it->second.col = it->second.pendingCol;
      it->second.row = it->second.pendingRow;
      it->second.hasPending = false;
      out.col = it->second.col;
      out.row = it->second.row;
      return true;
    }

    if (!s_connected)
    {
      s_lastError = "not connected";
      return false;
    }

    JsonDocument reqDoc;
    reqDoc["uuid"] = uuid;

    JsonDocument respDoc;
    String errOut;
    if (!doPost("/tokens/move/confirm", reqDoc, respDoc, errOut))
    {
      s_lastError = errOut;
      return false;
    }

    JsonObject token = respDoc["token"].as<JsonObject>();
    if (token.isNull())
    {
      s_lastError = "unexpected /tokens/move/confirm response shape";
      return false;
    }

    out.col = token["col"] | 0;
    out.row = token["row"] | 0;
    return true;
  }

  bool cancelMove(const String &uuid, bool *outCleared)
  {
    if (s_testMode)
    {
      auto it = mockTokenStates().find(uuid);
      bool cleared = (it != mockTokenStates().end()) && it->second.hasPending;
      if (cleared)
        it->second.hasPending = false;
      if (outCleared)
        *outCleared = cleared;
      return true;
    }

    if (!s_connected)
    {
      s_lastError = "not connected";
      return false;
    }

    JsonDocument reqDoc;
    reqDoc["uuid"] = uuid;

    JsonDocument respDoc;
    String errOut;
    if (!doPost("/tokens/move/cancel", reqDoc, respDoc, errOut))
    {
      s_lastError = errOut;
      return false;
    }

    if (outCleared)
      *outCleared = respDoc["cleared"] | false;
    return true;
  }

  bool createCircle(const String &uuid, float radius, const String &color, uint32_t durationMs, String *outTemplateId)
  {
    if (s_testMode)
    {
      if (outTemplateId)
        *outTemplateId = "mock-template-" + uuid;
      return true;
    }

    if (!s_connected)
    {
      s_lastError = "not connected";
      return false;
    }

    JsonDocument reqDoc;
    reqDoc["uuid"] = uuid;
    if (radius > 0)
      reqDoc["radius"] = radius;
    if (color.length() > 0)
      reqDoc["color"] = color;
    if (durationMs > 0)
      reqDoc["durationMs"] = durationMs;

    JsonDocument respDoc;
    String errOut;
    if (!doPost("/tokens/circle", reqDoc, respDoc, errOut))
    {
      s_lastError = errOut;
      return false;
    }

    if (outTemplateId)
      *outTemplateId = respDoc["templateId"] | "";
    return true;
  }

  bool previewCircle(const String &uuid, float radius, const String &color, uint32_t timeoutMs)
  {
    if (s_testMode)
    {
      // Test mode has no real canvas to draw a preview on - this is purely
      // about exercising the calling code's success/failure path, so it
      // just always succeeds without any further bookkeeping.
      return true;
    }

    if (!s_connected)
    {
      s_lastError = "not connected";
      return false;
    }

    JsonDocument reqDoc;
    reqDoc["uuid"] = uuid;
    if (radius > 0)
      reqDoc["radius"] = radius;
    if (color.length() > 0)
      reqDoc["color"] = color;
    if (timeoutMs > 0)
      reqDoc["timeoutMs"] = timeoutMs;

    JsonDocument respDoc;
    String errOut;
    if (!doPost("/tokens/circle/preview", reqDoc, respDoc, errOut))
    {
      s_lastError = errOut;
      return false;
    }

    return true;
  }

  bool cancelCirclePreview(const String &uuid, bool *outCleared)
  {
    if (s_testMode)
    {
      if (outCleared)
        *outCleared = false; // nothing was ever actually drawn in test mode - see previewCircle()
      return true;
    }

    if (!s_connected)
    {
      s_lastError = "not connected";
      return false;
    }

    JsonDocument reqDoc;
    reqDoc["uuid"] = uuid;

    JsonDocument respDoc;
    String errOut;
    if (!doPost("/tokens/circle/cancel", reqDoc, respDoc, errOut))
    {
      s_lastError = errOut;
      return false;
    }

    if (outCleared)
      *outCleared = respDoc["cleared"] | false;
    return true;
  }

} // namespace RelayClient