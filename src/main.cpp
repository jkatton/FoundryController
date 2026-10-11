#include <Arduino.h>

#include <ESP8266WiFi.h>
#include <WiFiManager.h>
#include <ArduinoOTA.h>
#include <ESP8266WiFiMulti.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPUpdateServer.h>

#include "config.h"
#include "Types.h"
#include "InputManager.h"
#include "Storage.h"
#include "RelayClient.h"
#include "Display.h"

// ===========================================================================
// Application state
// ===========================================================================
static AppState g_state = AppState::BOOT;

static std::vector<Character> g_characters;
static int g_selectedIndex = 0;

static uint16_t g_ipFields[5] = {192, 168, 1, 101, RELAY_DEFAULT_PORT};
static int g_ipFieldIndex = 0;

static String g_errorMessage;
static AppState g_stateAfterError = AppState::IP_ENTRY;

static unsigned long g_lastPollMs = 0;

// Settings carousel (Types::AppSettings). g_settings is the last
// saved/applied value; g_settingsDraft is what the SETTINGS screen is
// currently editing - changes preview live (see SettingDescriptor below)
// but only get written to Storage and adopted into g_settings when the
// user long-presses A to save. Long-pressing B instead discards the draft
// and reverts the live preview back to g_settings.
static AppSettings g_settings;
static AppSettings g_settingsDraft;
static int g_settingsIndex = 0;

// Detail screen editing state
static Character g_activeChar;
static HpAddTarget g_addTarget = HpAddTarget::NORMAL; // which pool a positive turn adds to
static int g_deltaValue = 0;                          // pending signed adjustment from the encoder, always relative
static bool g_dirty = false;                          // user has an uncommitted edit; pause auto-refresh overwrite
static bool g_sending = false;
static bool g_justSent = false;
static unsigned long g_sentBannerUntil = 0;

// HP editing and a pending move are now *both* always live on the detail
// screen at once - a joystick nudge no longer suspends HP editing, and
// turning the encoder no longer cancels a pending move. g_lastModified
// just tracks which of the two the shared "sub-display" area (the arrow/
// status rows at the bottom of the screen) is currently showing, so it's
// always whichever the user touched most recently. Button A acts on
// whichever one is currently shown (see handleCharacterDetail()).
enum class DetailSubDisplay : uint8_t
{
  HP,
  MOVE
};
static DetailSubDisplay g_lastModified = DetailSubDisplay::HP;

// Move: first joystick nudge on the detail screen starts accumulating a
// token move to preview; every nudge after that (including the first)
// adds onto the running total and resends a fresh preview. dCol/dRow are
// the running total offset from the token's actual position (see
// RelayClient::previewMoveRelative()) since the move started - each nudge
// resends the new total, not just the latest press, since the relay
// always computes relative to the token's real (unmoved) position rather
// than the last preview. Exited by confirming (Button A while the move is
// the shown sub-display - see confirmMoveAndReturn()) or by a long press
// of Button A while it's shown, which cancels it (see handleCharacterDetail()).
static bool g_moveMode = false;
static int g_moveDCol = 0;
static int g_moveDRow = 0;
static float g_moveDistance = 0;
static String g_moveUnits;
static bool g_moveSending = false; // a previewMoveRelative()/confirmMove() call is in flight

// Circle: entered from the detail screen by a short press of Button B,
// which hands the encoder over to dialing a circle's radius
// (g_circleEditing) centered on the active character's token, without
// touching whichever of HP-edit or move is the current sub-display -
// exiting radius-editing resumes whichever of those was showing,
// unchanged. Once in radius-editing, the two presses of Button B mean
// different things: a short press removes whatever circle is currently
// drawn (cancelling the live preview) but *stays* in radius-editing and
// keeps the dialed-in radius exactly as it was, so a fresh circle can be
// dialed right back up without starting over; a long press does the
// opposite - it exits radius-editing (hands the encoder back) but leaves
// the circle exactly as drawn, persisting on the map. Either way the
// radius itself is remembered (see persistCircleRadius()) so it carries
// over between reboots and different characters. Dialing the radius all
// the way down to zero with the encoder is a third, distinct way out -
// see the zero-radius handling in handleCharacterDetail(), which resets
// the remembered radius back to the default instead of remembering zero.
static bool g_circleEditing = false;
static bool g_circleDrawn = false;                   // true whenever a circle preview currently exists on the relay's map for this character
static float g_circleRadius = CIRCLE_RADIUS_DEFAULT; // seeded from g_settings.circleRadius in setup(); persisted (see
                                                     // persistCircleRadius()) whenever it changes, so it carries over
                                                     // between reboots and different characters
static bool g_circleSending = false;                 // a previewCircle() call is in flight
static bool g_circleRadiusDirty = false;             // radius changed since the last preview send - see CIRCLE_RADIUS_DEBOUNCE_MS
static unsigned long g_circleLastRadiusChangeMs = 0;

// ===========================================================================
// Helpers
// ===========================================================================

// Forward-declared so sendMovePreview() below - defined earlier in the file
// than currentCircleIndicator() itself - can include the circle indicator
// in its display refreshes. That matters now that a circle can be drawn
// (g_circleDrawn) independently of whether it's currently being edited, so
// a move nudge re-centering it (see nudgeMove()) can happen at any time:
// without this, sendMovePreview()'s own display calls would momentarily
// drop the on-screen circle readout until the subsequent
// sendCirclePreview() call redraws it. Harmless when no circle is drawn
// at all, where this always evaluates to
// an inactive indicator anyway - same as the implicit default it's
// replacing.
//
// redrawDetailScreen() is forward-declared for the same reason: its own
// definition comes later in the file than sendCurrentHp() and
// confirmMoveAndReturn(), both of which now call it directly on their
// no-op/zero-change early-return paths.
static CircleIndicator currentCircleIndicator();
static MovementIndicator currentMoveIndicator();
static void sendCirclePreview();
static void redrawDetailScreen();

static void goToError(const String &msg, AppState after)
{
  g_errorMessage = msg;
  g_stateAfterError = after;
  g_state = AppState::ERROR_SCREEN;
  Display::showError(msg);
}

// ---------------------------------------------------------------------
// Settings carousel
// ---------------------------------------------------------------------
// One descriptor per row, in the order they appear in the carousel. Each
// setting owns its own encoder-delta -> value logic and value -> display
// string formatting; the carousel itself (handleSettings() below, and
// Display::showSettings()) has no per-setting knowledge, so adding a new
// setting is just: add a field to AppSettings (Types.h), extend
// Storage::loadSettings/saveSettings to persist it, and add one entry
// below.
struct SettingDescriptor
{
  const char *name;
  void (*applyDelta)(AppSettings &s, int32_t delta); // called with nonzero encoder delta; should live-preview via Display::set*()
  String (*valueString)(const AppSettings &s);
  void (*onActivate)(); // optional (null for plain settings): run when Button B is short-pressed on this row
};

static void settingApplyRotation(AppSettings &s, int32_t delta)
{
  int v = ((int)s.screenRotation + delta) % 4;
  if (v < 0)
    v += 4;
  s.screenRotation = (uint8_t)v;
  Display::setRotation(s.screenRotation);
}

static String settingValueRotation(const AppSettings &s)
{
  static const char *labels[4] = {"0", "90", "180", "270"};
  return String(labels[s.screenRotation & 0x03]) + " deg";
}

static void settingApplyGaugeStyle(AppSettings &s, int32_t delta)
{
  // Only two states - any turn just flips it, regardless of direction or
  // magnitude.
  if (delta != 0)
  {
    s.gaugeStyleFlat = !s.gaugeStyleFlat;
    Display::setGaugeStyleFlat(s.gaugeStyleFlat);
  }
}

static String settingValueGaugeStyle(const AppSettings &s)
{
  return s.gaugeStyleFlat ? "Flat" : "Gradient";
}

static void settingApplySerialDebug(AppSettings &s, int32_t delta)
{
  // Only two states - any turn just flips it, regardless of direction or
  // magnitude.
  if (delta != 0)
  {
    s.serialDebug = !s.serialDebug;
    InputManager::setDebugEnabled(s.serialDebug);
  }
}

static String settingValueSerialDebug(const AppSettings &s)
{
  return s.serialDebug ? "On" : "Off";
}

static void settingApplyRelayTestMode(AppSettings &s, int32_t delta)
{
  // Only two states - any turn just flips it, regardless of direction or
  // magnitude.
  if (delta != 0)
  {
    s.relayTestMode = !s.relayTestMode;
    RelayClient::setTestMode(s.relayTestMode);
  }
}

static String settingValueRelayTestMode(const AppSettings &s)
{
  return s.relayTestMode ? "On" : "Off";
}

static void settingApplyTableOrientation(AppSettings &s, int32_t delta)
{
  int v = ((int)s.tableOrientation + delta) % 4;
  if (v < 0)
    v += 4;
  s.tableOrientation = (uint8_t)v;
  // Purely a move-command transform applied at send time (see
  // rotateForTableOrientation() below) - unlike screen rotation or gauge
  // style, there's nothing on this screen itself to live-preview.
}

static String settingValueTableOrientation(const AppSettings &s)
{
  static const char *labels[4] = {"^", "<", "v", ">"};
  return "Table N is: " + String(labels[s.tableOrientation & 0x03]);
}

static void settingApplyMoveButtons(AppSettings &s, int32_t delta)
{
  // Only two states - any turn just flips it, regardless of direction or
  // magnitude.
  if (delta != 0)
  {
    s.moveButtonsEnabled = !s.moveButtonsEnabled;
  }
}

static String settingValueMoveButtons(const AppSettings &s)
{
  return s.moveButtonsEnabled ? "On" : "Off";
}

static void settingApplyCircleEnabled(AppSettings &s, int32_t delta)
{
  // Only two states - any turn just flips it, regardless of direction or
  // magnitude.
  if (delta != 0)
  {
    s.circleEnabled = !s.circleEnabled;
  }
}

static String settingValueCircleEnabled(const AppSettings &s)
{
  return s.circleEnabled ? "On" : "Off";
}

static void settingApplySwapButtons(AppSettings &s, int32_t delta)
{
  // Only two states - any turn just flips it, regardless of direction or
  // magnitude. Deliberately NOT applied to InputManager here (unlike e.g.
  // settingApplySerialDebug above) - see AppSettings::swapButtons' own
  // comment (Types.h) for why this one only takes effect on save, in
  // exitSettingsSave() below, rather than live-previewing.
  if (delta != 0)
  {
    s.swapButtons = !s.swapButtons;
  }
}

static String settingValueSwapButtons(const AppSettings &s)
{
  return s.swapButtons ? "On" : "Off";
}

static void startOta(); // defined below, alongside handleSettings()
static void startAddWifi();
static void openWifiList();

static void settingApplyNoop(AppSettings &s, int32_t delta)
{
  // Action row, not an editable value - handled via onActivate.
}

static String pressBLabel()
{
  // Names whichever physical button currently performs the "B" role.
  return String("Press ") + (InputManager::buttonsSwapped() ? BUTTON_A_LABEL : BUTTON_B_LABEL);
}

static String settingValueAddWifi(const AppSettings &s)
{
  return pressBLabel();
}

static String settingValueSavedWifi(const AppSettings &s)
{
  return pressBLabel();
}

static void settingApplyOta(AppSettings &s, int32_t delta)
{
  // Action row, not an editable value - starting OTA is done via onActivate.
}

static String settingValueOta(const AppSettings &s)
{
  // Names whichever physical button currently performs the "B" role.
  return String("Press ") + (InputManager::buttonsSwapped() ? BUTTON_A_LABEL : BUTTON_B_LABEL);
}

static const SettingDescriptor SETTINGS_TABLE[] = {
// Player-facing
    {"Screen rotation", settingApplyRotation, settingValueRotation},
    {"Table Orientation", settingApplyTableOrientation, settingValueTableOrientation},
    {"Move Buttons", settingApplyMoveButtons, settingValueMoveButtons},
    {"Circle Feature", settingApplyCircleEnabled, settingValueCircleEnabled},
    {"Swap X/O", settingApplySwapButtons, settingValueSwapButtons},
    {"HP gauge style", settingApplyGaugeStyle, settingValueGaugeStyle},
    // Debug / test
    {"Serial Debug Output", settingApplySerialDebug, settingValueSerialDebug},
    {"Local Relay Test Mode", settingApplyRelayTestMode, settingValueRelayTestMode},
    // Network
    {"Saved WiFi", settingApplyNoop, settingValueSavedWifi, openWifiList},
    {"Add WiFi Network", settingApplyNoop, settingValueAddWifi, startAddWifi},
    // Update
    {"Firmware Update", settingApplyOta, settingValueOta, startOta},
};
static const int SETTINGS_COUNT = sizeof(SETTINGS_TABLE) / sizeof(SETTINGS_TABLE[0]);

// Rotates a (col, row) vector, in screen coordinates (col=+right,
// row=+down), by `turns` 90-degree clockwise quarter turns (0-3). Shared
// by rotateForScreenRotation() and rotateForTableOrientation() below -
// both are the same quarter-turn transform, just triggered by different
// settings and applied at different points in the move pipeline.
static void rotateQuarterTurnsCW(int col, int row, uint8_t turns, int &outCol, int &outRow)
{
  switch (turns & 0x03)
  {
  case 1: // 90 deg clockwise
    outCol = -row;
    outRow = col;
    break;
  case 2: // 180 deg
    outCol = -col;
    outRow = -row;
    break;
  case 3: // 270 deg clockwise (90 deg counter-clockwise)
    outCol = row;
    outRow = -col;
    break;
  default: // 0 deg
    outCol = col;
    outRow = row;
    break;
  }
}

// Remaps a physical joystick direction press (e.g. joyUp -> (0,-1)) into
// the logical (dCol, dRow) grid delta the rest of the move pipeline
// works with, compensating for the currently-applied
// g_settings.screenRotation so the four physical presses always match
// whatever is visually up/down/left/right on the (possibly rotated)
// screen, rather than a fixed hardware axis. screenRotation's units (0-3
// quarter turns) are the same ones passed to Display::setRotation(), so
// this uses g_settings (the saved/applied value) rather than
// g_settingsDraft - a rotation only being previewed on the settings
// screen shouldn't yet change how the detail screen's buttons behave.
// The result of this feeds nudgeMove(), so it's what accumulates into
// g_moveDCol/g_moveDRow and shows up in the on-screen arrows - table
// orientation (rotateForTableOrientation()) is a second, independent
// rotation applied after this one, only to what's sent to the relay.
static void rotateForScreenRotation(int col, int row, int &outCol, int &outRow)
{
  // Compensating for the screen's rotation means applying the INVERSE of
  // that rotation to the input, not the same one - if the display's
  // content is rotated 90 deg CW, a physical "up" press needs to be
  // rotated 90 deg CCW (i.e. 270 deg CW) to still land on visual "up".
  // 0 and 180 deg are their own inverse (0 and 2 quarter turns undo
  // themselves), which is why those two happened to work even with the
  // rotation applied forwards; 90 and 270 are each other's inverse, so
  // applying screenRotation directly sent those two backwards.
  uint8_t inverseTurns = (4 - (g_settings.screenRotation & 0x03)) & 0x03;
  rotateQuarterTurnsCW(col, row, inverseTurns, outCol, outRow);
}

// Rotates a joystick-accumulated (dCol, dRow) delta by g_settings.tableOrientation
// quarter turns clockwise, in screen coordinates (col=+right, row=+down),
// before it's sent to the relay as a move command - see
// AppSettings::tableOrientation's own comment for why. Only ever applied
// to what's actually transmitted (see sendMovePreview()) - the on-screen
// accumulated g_moveDCol/g_moveDRow (arrows, distance-so-far) always
// reflect the screen-rotation-compensated presses (see
// rotateForScreenRotation()), not this second rotation, since that's what
// the user actually intended by pressing that direction.
static void rotateForTableOrientation(int dCol, int dRow, int &outCol, int &outRow)
{
  rotateQuarterTurnsCW(dCol, dRow, g_settings.tableOrientation, outCol, outRow);
}

static void renderSettingsScreen()
{
  std::vector<String> names, values;
  names.reserve(SETTINGS_COUNT);
  values.reserve(SETTINGS_COUNT);
  for (int i = 0; i < SETTINGS_COUNT; i++)
  {
    names.push_back(SETTINGS_TABLE[i].name);
    values.push_back(SETTINGS_TABLE[i].valueString(g_settingsDraft));
  }
  Display::showSettings(names, values, g_settingsIndex);
}

static void enterIpEntryDefaults()
{
  // Pre-fill with the last saved address if we have one, otherwise a
  // sensible LAN default.
  String host;
  uint16_t port;
  if (Storage::loadRelayAddress(host, port))
  {
    int a, b, c, d;
    if (sscanf(host.c_str(), "%d.%d.%d.%d", &a, &b, &c, &d) == 4)
    {
      g_ipFields[0] = a;
      g_ipFields[1] = b;
      g_ipFields[2] = c;
      g_ipFields[3] = d;
    }
    g_ipFields[4] = port;
  }
  g_ipFieldIndex = 0;
  g_state = AppState::IP_ENTRY;
  Display::showIpEntry(g_ipFields, g_ipFieldIndex, g_settings.relayTestMode);
}

static void enterSettings()
{
  g_settingsDraft = g_settings; // start editing from the last saved/applied values
  g_settingsIndex = 0;
  g_state = AppState::SETTINGS;
  renderSettingsScreen();
}

static void exitSettingsDiscard()
{
  // Revert whatever got live-previewed while editing (rotation, gauge
  // style, debug output, relay test mode) back to what's actually saved,
  // then head back to IP entry.
  Display::setRotation(g_settings.screenRotation);
  Display::setGaugeStyleFlat(g_settings.gaugeStyleFlat);
  InputManager::setDebugEnabled(g_settings.serialDebug);
  RelayClient::setTestMode(g_settings.relayTestMode);
  enterIpEntryDefaults();
}

static void exitSettingsSave()
{
  g_settings = g_settingsDraft;
  Storage::saveSettings(g_settings);
  // Unlike rotation/gauge style/debug/relay test mode above (applied live
  // as they're edited), swapButtons is only applied here, once the draft
  // is actually saved - see AppSettings::swapButtons' own comment.
  InputManager::setButtonsSwapped(g_settings.swapButtons);
  enterIpEntryDefaults();
}

static bool tryConnectRelay(const String &host, uint16_t port)
{
  Display::showRelayConnecting(host, port);
  RelayClient::begin(host, port);
  return RelayClient::connect();
}

static void loadCharacterList()
{
  g_state = AppState::LOADING_CHARACTERS;
  Display::showLoadingCharacters();

  if (!RelayClient::fetchCharacters(g_characters))
  {
    goToError("fetch failed: " + RelayClient::lastError(), AppState::IP_ENTRY);
    return;
  }

  g_selectedIndex = 0;
  g_state = AppState::CHARACTER_LIST;
  Display::showCharacterList(g_characters, g_selectedIndex);
  g_lastPollMs = millis();
}

// RelayClient no longer exposes a single-character fetch - the full list is
// cheap enough (a handful of characters) that refreshing just one pulls
// everyone and picks the match out client-side.
static bool fetchCharacterByUuid(const String &uuid, Character &out)
{
  std::vector<Character> all;
  if (!RelayClient::fetchCharacters(all))
    return false; // RelayClient::lastError() already set

  for (auto &c : all)
  {
    if (c.uuid == uuid)
    {
      out = c;
      return true;
    }
  }
  return false;
}

static void enterDetailForSelected()
{
  if (g_characters.empty())
    return;
  g_activeChar = g_characters[g_selectedIndex];
  g_addTarget = HpAddTarget::NORMAL;
  g_deltaValue = 0;
  g_dirty = false;
  g_sending = false;
  g_justSent = false;
  g_lastModified = DetailSubDisplay::HP;
  g_moveMode = false; // defensive reset - any pending move should already be exited by the time we leave this screen
  g_moveDCol = 0;
  g_moveDRow = 0;
  g_moveDistance = 0;
  g_moveUnits = "";
  g_moveSending = false;
  g_circleEditing = false; // defensive reset - same reasoning as move mode above
  g_circleDrawn = false;   // this character's own circle (if any) is left alone server-side - it persists on the map,
                           // this just stops the *new* character's screen from claiming one it doesn't have
  g_circleSending = false; // (g_circleRadius intentionally not reset here - it's meant to carry over between characters)
  g_state = AppState::CHARACTER_DETAIL;
  Display::showCharacterDetail(g_activeChar, g_addTarget, g_activeChar.hp, g_activeChar.tempHp, g_deltaValue, false, false);
}

// Applies g_deltaValue on top of g_activeChar's saved hp/tempHp, following
// the add-target toggle for positive turns and the temp-first rule for
// negative ones, without mutating any state. Used both to preview the
// pending edit on screen and, on send, as the values written to the relay.
struct PendingHp
{
  int hp;
  int tempHp;
};

static PendingHp computePendingHp()
{
  PendingHp p;
  p.hp = g_activeChar.hp;
  p.tempHp = g_activeChar.tempHp;

  if (g_deltaValue > 0)
  {
    if (g_addTarget == HpAddTarget::TEMP)
    {
      p.tempHp += g_deltaValue;
    }
    else
    {
      p.hp += g_deltaValue;
      if (p.hp > g_activeChar.maxHp)
        p.hp = g_activeChar.maxHp;
    }
  }
  else if (g_deltaValue < 0)
  {
    int dmg = -g_deltaValue;
    int fromTemp = (dmg < p.tempHp) ? dmg : p.tempHp;
    p.tempHp -= fromTemp;
    p.hp -= (dmg - fromTemp);
    if (p.hp < 0)
      p.hp = 0;
  }

  return p;
}

static int currentDisplayHp() { return computePendingHp().hp; }
static int currentDisplayTempHp() { return computePendingHp().tempHp; }

// ===========================================================================
// State handlers
// ===========================================================================

static void handleIpEntry()
{
  int32_t delta = InputManager::readEncoderDelta(InputManager::EncoderProfile::IP_ENTRY);
  if (delta != 0)
  {
    uint16_t &field = g_ipFields[g_ipFieldIndex];
    int maxVal = (g_ipFieldIndex < 4) ? 255 : 65535;
    int newVal = (int)field + delta;
    if (newVal < 0)
      newVal = maxVal;
    if (newVal > maxVal)
      newVal = 0;
    field = (uint16_t)newVal;
    Display::showIpEntry(g_ipFields, g_ipFieldIndex, g_settings.relayTestMode);
  }

  ButtonEvent a = InputManager::pollButtonA();
  if (a == ButtonEvent::SHORT_PRESS)
  {
    g_ipFieldIndex = (g_ipFieldIndex + 1) % 5;
    Display::showIpEntry(g_ipFields, g_ipFieldIndex, g_settings.relayTestMode);
  }
  else if (a == ButtonEvent::LONG_PRESS)
  {
    String host = String(g_ipFields[0]) + "." + String(g_ipFields[1]) + "." +
                  String(g_ipFields[2]) + "." + String(g_ipFields[3]);
    uint16_t port = g_ipFields[4];

    g_state = AppState::RELAY_TESTING_ENTRY;
    if (tryConnectRelay(host, port))
    {
      Storage::saveRelayAddress(host, port);
      loadCharacterList();
    }
    else
    {
      goToError("connect failed: " + RelayClient::lastError(), AppState::IP_ENTRY);
    }
  }

  // Button B: long-press opens the settings carousel (screen rotation, HP
  // gauge style, ...). See enterSettings()/handleSettings().
  ButtonEvent b = InputManager::pollButtonB();
  if (b == ButtonEvent::LONG_PRESS)
  {
    enterSettings();
    return;
  }
}

// ---------------------------------------------------------------------
// WiFi: multiple saved networks
// ---------------------------------------------------------------------
// Opens the WiFiManager config portal and, if it ends up connected, adds
// that network to the saved list. startConfigPortal() rather than
// autoConnect(): autoConnect would first waste time re-trying the one
// network the SDK remembers, which the caller has already dealt with.
static bool runWifiPortal()
{
  Display::showWifiPortalHint();
  WiFiManager wm;
  wm.setConfigPortalTimeout(WIFI_PORTAL_TIMEOUT_S);
  bool ok = wm.startConfigPortal("FoundryController-Setup"); // blocks until configured or timed out
  if (ok && WiFi.status() == WL_CONNECTED)
  {
    Storage::addWifiNetwork(WiFi.SSID(), WiFi.psk());
    return true;
  }
  return false;
}

// Boot-time connect: try every saved network (ESP8266WiFiMulti scans and
// joins the strongest one in range), and only open the config portal if
// none of them connect.
static bool connectWifi()
{
  WiFi.mode(WIFI_STA);

  std::vector<WifiCred> nets;
  Storage::loadWifiNetworks(nets);

  // One-time migration: if there's no wifi.cfg yet but the SDK still
  // remembers a network from before multi-network support, adopt it.
  if (nets.empty() && !Storage::wifiConfigExists() && WiFi.SSID().length() > 0)
  {
    Storage::addWifiNetwork(WiFi.SSID(), WiFi.psk());
    Storage::loadWifiNetworks(nets);
  }

  if (!nets.empty())
  {
    Display::showBoot("Looking for WiFi...");
    ESP8266WiFiMulti multi;
    for (size_t i = 0; i < nets.size(); i++)
      multi.addAP(nets[i].ssid.c_str(), nets[i].pass.c_str());
    if (multi.run(WIFI_MULTI_TIMEOUT_MS) == WL_CONNECTED)
      return true;
  }

  return runWifiPortal();
}

// Settings row: open the portal on demand to add another network, even
// though a saved one may be working. Always restarts afterwards so the
// normal boot-time connect picks the best network from the updated list
// (any unsaved settings draft is discarded, same as OTA).
static void startAddWifi()
{
  runWifiPortal();
  Display::showBoot("Restarting...");
  delay(1000);
  ESP.restart();
}

// ---- Saved WiFi list (reached from the "Saved WiFi" settings row) ----
// Encoder or A:next scrolls, B asks to delete the selected network and a
// second B press confirms, holding either button returns to settings.
// Deleting only edits the saved list - it takes effect at the next boot
// (if the deleted network is the one currently connected, the board stays
// on it until then).
static std::vector<WifiCred> g_wifiNets;
static int g_wifiIndex = 0;
static bool g_wifiDeleteArmed = false;

static void renderWifiList()
{
  std::vector<String> ssids, notes;
  String current = (WiFi.status() == WL_CONNECTED) ? WiFi.SSID() : String("");
  for (size_t i = 0; i < g_wifiNets.size(); i++)
  {
    ssids.push_back(g_wifiNets[i].ssid);
    notes.push_back((current.length() > 0 && g_wifiNets[i].ssid == current) ? String("connected") : String(""));
  }
  Display::showWifiList(ssids, notes, g_wifiIndex, g_wifiDeleteArmed);
}

static void openWifiList()
{
  Storage::loadWifiNetworks(g_wifiNets);
  g_wifiIndex = 0;
  g_wifiDeleteArmed = false;
  g_state = AppState::WIFI_LIST;
  renderWifiList();
}

static void handleWifiList()
{
  int n = (int)g_wifiNets.size();

  int32_t delta = InputManager::readEncoderDelta();
  if (delta != 0 && n > 0)
  {
    g_wifiIndex = (int)((((long)g_wifiIndex + delta) % n + n) % n);
    g_wifiDeleteArmed = false;
    renderWifiList();
  }

  ButtonEvent a = InputManager::pollButtonA();
  ButtonEvent b = InputManager::pollButtonB();

  if (a == ButtonEvent::LONG_PRESS || b == ButtonEvent::LONG_PRESS)
  {
    g_state = AppState::SETTINGS;
    renderSettingsScreen(); // settings draft/selection are untouched, so this just redraws where the user left off
    return;
  }

  if (a == ButtonEvent::SHORT_PRESS && n > 0)
  {
    g_wifiIndex = (g_wifiIndex + 1) % n;
    g_wifiDeleteArmed = false;
    renderWifiList();
  }

  if (b == ButtonEvent::SHORT_PRESS && n > 0)
  {
    if (!g_wifiDeleteArmed)
    {
      g_wifiDeleteArmed = true;
    }
    else
    {
      Storage::removeWifiNetwork(g_wifiNets[g_wifiIndex].ssid);
      Storage::loadWifiNetworks(g_wifiNets);
      if (g_wifiIndex >= (int)g_wifiNets.size())
        g_wifiIndex = (int)g_wifiNets.size() - 1;
      if (g_wifiIndex < 0)
        g_wifiIndex = 0;
      g_wifiDeleteArmed = false;
    }
    renderWifiList();
  }
}

static int g_otaLastPct = -1;

// Browser-based upload (http://<ip>/update) alongside ArduinoOTA's
// PlatformIO/espota listener. ArduinoOTA itself has no web page - it only
// speaks UDP/TCP to the espota tool - so this is what makes the IP shown on
// the OTA screen actually open in a browser.
static ESP8266WebServer g_otaWeb(80);
static ESP8266HTTPUpdateServer g_httpUpdater;

static void startOta()
{
  if (WiFi.status() != WL_CONNECTED)
    return;

  ArduinoOTA.setHostname("foundry-controller");
  // ArduinoOTA.setPassword("changeme"); // recommended on a shared network

  ArduinoOTA.onStart([]() {
    g_otaLastPct = -1;
    Display::showOtaStatus("Receiving update...", "Do not power off");
    Display::showOtaProgress(0);
  });
  ArduinoOTA.onProgress([](unsigned int done, unsigned int total) {
    int pct = (int)((done * 100UL) / total);
    if (pct != g_otaLastPct) // only repaint when the number changes
    {
      g_otaLastPct = pct;
      Display::showOtaProgress(pct);
    }
  });
  ArduinoOTA.onEnd([]() {
    Display::showOtaStatus("Update complete", "Rebooting...");
  });
  ArduinoOTA.onError([](ota_error_t err) {
    Display::showOtaStatus("Update failed", "Error " + String((int)err));
    delay(3000);
    ESP.restart();
  });

  ArduinoOTA.begin();

  // Modem sleep can make the board miss incoming connections/invitations.
  WiFi.setSleepMode(WIFI_NONE_SLEEP);

  g_httpUpdater.setup(&g_otaWeb, "/update"); // add ", \"user\", \"pass\"" here to require a login
  g_otaWeb.on("/", HTTP_GET, []() {
    g_otaWeb.sendHeader("Location", "/update");
    g_otaWeb.send(302, "text/plain", "");
  });
  g_otaWeb.begin();

  g_state = AppState::OTA_UPDATE;
  Display::showOtaStatus("http://" + WiFi.localIP().toString() + "/update", "or foundry-controller.local");
}

static void handleOta()
{
  ArduinoOTA.handle();
  g_otaWeb.handleClient();
  if (InputManager::pollButtonA() == ButtonEvent::LONG_PRESS ||
      InputManager::pollButtonB() == ButtonEvent::LONG_PRESS)
  {
    ESP.restart(); // simplest clean way out; any unsaved settings draft is discarded
  }
}

static void handleSettings()
{
  int32_t delta = InputManager::readEncoderDelta();
  if (delta != 0 && SETTINGS_COUNT > 0)
  {
    SETTINGS_TABLE[g_settingsIndex].applyDelta(g_settingsDraft, delta);
    renderSettingsScreen();
  }

  ButtonEvent a = InputManager::pollButtonA();
  if (a == ButtonEvent::SHORT_PRESS)
  {
    if (SETTINGS_COUNT > 0)
      g_settingsIndex = (g_settingsIndex + 1) % SETTINGS_COUNT;
    renderSettingsScreen();
  }
  else if (a == ButtonEvent::LONG_PRESS)
  {
    exitSettingsSave();
    return;
  }

  ButtonEvent b = InputManager::pollButtonB();
  if (b == ButtonEvent::SHORT_PRESS)
  {
    if (SETTINGS_TABLE[g_settingsIndex].onActivate)
      SETTINGS_TABLE[g_settingsIndex].onActivate();
    if (g_state == AppState::SETTINGS) // startOta() etc. switch screens; don't paint over them
      renderSettingsScreen();
  }
  else if (b == ButtonEvent::LONG_PRESS)
  {
    exitSettingsDiscard();
    return;
  }
}

static void handleCharacterList()
{
  int32_t delta = InputManager::readEncoderDelta();
  if (delta != 0 && !g_characters.empty())
  {
    int n = g_characters.size();
    g_selectedIndex = ((g_selectedIndex + delta) % n + n) % n;
    Display::showCharacterList(g_characters, g_selectedIndex);
  }

  ButtonEvent a = InputManager::pollButtonA();
  if (a == ButtonEvent::SHORT_PRESS)
  {
    enterDetailForSelected();
    return;
  }

  else if (a == ButtonEvent::DOUBLE_PRESS)
  {
    // Reconfigure server address from scratch.
    RelayClient::begin("", 0);
    enterIpEntryDefaults();
    return;
  }
  // else if (a == ButtonEvent::LONG_PRESS) {
  //   loadCharacterList(); // manual refresh
  // }

  ButtonEvent b = InputManager::pollButtonB();
  if (b == ButtonEvent::LONG_PRESS)
  {
    // Reconfigure server address from scratch.
    RelayClient::begin("", 0);
    enterIpEntryDefaults();
    return;
  }

  // Background polling
  if (millis() - g_lastPollMs > CHARACTER_POLL_MS)
  {
    g_lastPollMs = millis();
    std::vector<Character> fresh;
    if (RelayClient::fetchCharacters(fresh) && !fresh.empty())
    {
      if (g_selectedIndex >= (int)fresh.size())
        g_selectedIndex = fresh.size() - 1;
      g_characters = fresh;
      Display::showCharacterList(g_characters, g_selectedIndex);
    }
    // Silently ignore poll failures here; the user can long-press A to
    // retry manually, and we don't want a transient network hiccup to
    // bounce them out of the list.
  }
}

static void sendCurrentHp()
{
  int pendingHp = currentDisplayHp();
  int pendingTempHp = currentDisplayTempHp();

  // Nothing would actually change on the relay - e.g. the encoder swung
  // but got clamped right back to where HP/temp HP already were (already
  // at max, or already at 0), or g_deltaValue is 0 outright. Skip the
  // network round trip entirely rather than sending a no-op update: no
  // "sending"/"Sent!" UI, no request. Still hands the shared sub-display
  // over to a queued move (same as the real-send path below) so a short
  // press of A reliably gets you to whichever of HP/move actually has
  // something pending, even when this particular press turned out to be
  // a no-op.
  if (pendingHp == g_activeChar.hp && pendingTempHp == g_activeChar.tempHp)
  {
    g_deltaValue = 0;
    g_dirty = false;
    if (g_moveMode)
    {
      g_lastModified = DetailSubDisplay::MOVE;
    }
    redrawDetailScreen();
    return;
  }

  g_sending = true;
  Display::showCharacterDetail(g_activeChar, g_addTarget, pendingHp, pendingTempHp, g_deltaValue, true, false,
                               currentMoveIndicator(), currentCircleIndicator());

  bool ok = RelayClient::updateCharacterHp(g_activeChar.uuid, pendingHp, pendingTempHp);

  g_sending = false;
  if (ok)
  {
    g_activeChar.hp = pendingHp;
    g_activeChar.tempHp = pendingTempHp;
    g_deltaValue = 0;
    g_dirty = false;
    g_justSent = true;
    g_sentBannerUntil = millis() + 1200;
    // Reflect the change in the cached list too, so returning to the list
    // shows the updated value immediately.
    if (g_selectedIndex >= 0 && g_selectedIndex < (int)g_characters.size())
    {
      g_characters[g_selectedIndex].hp = pendingHp;
      g_characters[g_selectedIndex].tempHp = pendingTempHp;
    }
    // A move can be queued in the background while HP editing is what's
    // shown (see the encoder branch in handleCharacterDetail() and
    // currentMoveIndicator()'s comment) - once the HP update is actually
    // sent, there's nothing more to look at on the HP side, so hand the
    // shared sub-display over to the pending move instead of continuing
    // to sit on the now-quiet HP view.
    if (g_moveMode)
    {
      g_lastModified = DetailSubDisplay::MOVE;
    }
  }
  else
  {
    goToError("update failed: " + RelayClient::lastError(), AppState::CHARACTER_LIST);
    return;
  }
  Display::showCharacterDetail(g_activeChar, g_addTarget, pendingHp, pendingTempHp, g_deltaValue, false, g_justSent,
                               currentMoveIndicator(), currentCircleIndicator());
}

// Resets move-mode state and returns to normal HP editing. If
// cancelOnServer is true, also tells the relay to discard whatever
// pending target is out there for the active character. Note this is
// best-effort but NOT self-healing: the relay module no longer expires
// move previews on its own, so if this call fails (e.g. a network
// hiccup), the preview line/marker is left showing on the map until
// something else clears it for this uuid (a later previewMove(), or a
// manual cancel from Foundry).
static void exitMoveMode(bool cancelOnServer)
{
  if (cancelOnServer && g_moveMode)
  {
    RelayClient::cancelMove(g_activeChar.uuid);
  }
  g_moveMode = false;
  g_moveDCol = 0;
  g_moveDRow = 0;
  g_moveDistance = 0;
  g_moveUnits = "";
  g_moveSending = false;
}

static MovementIndicator currentMoveIndicator()
{
  MovementIndicator m;
  // Only show the move sub-display when there's actually a pending move
  // AND it's the one the user touched most recently - HP editing can be
  // "ahead" of a still-pending move (see the encoder branch in
  // handleCharacterDetail()), in which case the move stays live in the
  // background without being drawn here until it's nudged again.
  m.active = g_moveMode && (g_lastModified == DetailSubDisplay::MOVE);
  m.dCol = g_moveDCol;
  m.dRow = g_moveDRow;
  m.distance = g_moveDistance;
  m.units = g_moveUnits;
  m.sending = g_moveSending;
  return m;
}

// Sends the current accumulated (g_moveDCol, g_moveDRow) total as a fresh
// preview. Called on every nudge, always with the running total rather
// than just the latest press - see the g_moveMode comment up top for why
// that's the correct thing to resend.
static void sendMovePreview()
{
  g_moveSending = true;
  Display::showCharacterDetail(g_activeChar, g_addTarget, currentDisplayHp(), currentDisplayTempHp(), g_deltaValue, false, false, currentMoveIndicator(), currentCircleIndicator());

  // g_moveDCol/g_moveDRow are the raw, unrotated running total in screen
  // terms (what the joystick actually accumulated, and what's shown on
  // screen) - rotateForTableOrientation() only transforms the copy that
  // actually goes out over the wire, so the on-screen arrows/distance
  // keep matching what the user pressed regardless of table orientation.
  int sendDCol, sendDRow;
  rotateForTableOrientation(g_moveDCol, g_moveDRow, sendDCol, sendDRow);

  RelayClient::MovePreviewResult result;
  bool ok = RelayClient::previewMoveRelative(g_activeChar.uuid, sendDCol, sendDRow, result);

  g_moveSending = false;

  if (!ok)
  {
    // The preview call itself is what failed - there's no pending target
    // on the server to clean up.
    exitMoveMode(false);
    g_lastModified = DetailSubDisplay::HP;
    goToError("move failed: " + RelayClient::lastError(), AppState::CHARACTER_LIST);
    return;
  }

  g_moveDistance = result.distance;
  g_moveUnits = result.units;
  Display::showCharacterDetail(g_activeChar, g_addTarget, currentDisplayHp(), currentDisplayTempHp(), g_deltaValue, false, false, currentMoveIndicator(), currentCircleIndicator());
}

// First direction press on the detail screen starts a pending move; every
// press after that (including the first) accumulates onto the running
// total, resends a fresh preview, and switches the shared sub-display to
// show the move (see g_lastModified). If a circle is currently drawn on
// the map, it's re-sent afterward so it keeps tracking the move target
// live - this covers both actively dialing the radius and just having
// left one drawn (see g_circleDrawn's own comment), which is why this no
// longer needs a separate "during circle mode" variant of nudging.
static void nudgeMove(int dCol, int dRow)
{
  if (!g_moveMode)
  {
    g_moveMode = true;
    g_moveDCol = 0;
    g_moveDRow = 0;
    g_moveDistance = 0;
    g_moveUnits = "";
  }
  g_moveDCol += dCol;
  g_moveDRow += dRow;
  g_lastModified = DetailSubDisplay::MOVE;
  g_justSent = false; // a fresh move nudge supersedes any lingering "Sent!" banner from a previous HP/move send

  if (g_moveDCol == 0 && g_moveDRow == 0)
  {
    // Net accumulated offset is back to zero (the last few nudges
    // cancelled out) - there's nothing left to preview, so drop the
    // pending move entirely (telling the relay to discard its preview
    // target) and fall back to the HP sub-display, rather than sitting
    // on a move screen reading "0 ft" with no arrow.
    exitMoveMode(true);
    g_lastModified = DetailSubDisplay::HP;
    Display::showCharacterDetail(g_activeChar, g_addTarget, currentDisplayHp(), currentDisplayTempHp(), g_deltaValue, false, false, currentMoveIndicator(), currentCircleIndicator());

    if (g_circleDrawn && g_state == AppState::CHARACTER_DETAIL)
    {
      // Re-center the circle back on the token's real (unmoved)
      // position now that the pending move has been cancelled.
      sendCirclePreview();
    }
    return;
  }

  sendMovePreview();

  if (g_circleDrawn && g_state == AppState::CHARACTER_DETAIL)
  {
    // sendMovePreview() above hit an error and already navigated away if
    // g_state isn't CHARACTER_DETAIL anymore - nothing left to re-center.
    sendCirclePreview();
  }
}

// Commits whatever's currently previewed and returns to normal HP
// editing, reusing the same "Sent!" banner mechanism sendCurrentHp() uses
// so a completed move gets the same brief on-screen confirmation an HP
// update does.
static void confirmMoveAndReturn()
{
  // Nothing would actually move - the accumulated offset nets out to
  // zero. In normal use nudgeMove() already catches this itself (a nudge
  // that brings the total back to (0,0) exits move mode outright, so this
  // function wouldn't even be reached), but guarding here too means a
  // confirm never fires a no-op move at the relay even if some future
  // path reaches this function with a stale zero offset. Best-effort
  // cancel rather than confirm, same as nudgeMove()'s own zero handling.
  if (g_moveDCol == 0 && g_moveDRow == 0)
  {
    exitMoveMode(true);
    g_lastModified = DetailSubDisplay::HP;
    redrawDetailScreen();
    return;
  }

  g_moveSending = true;
  Display::showCharacterDetail(g_activeChar, g_addTarget, currentDisplayHp(), currentDisplayTempHp(), g_deltaValue, false, false,
                               currentMoveIndicator(), currentCircleIndicator());

  RelayClient::MoveResult result;
  bool ok = RelayClient::confirmMove(g_activeChar.uuid, result);

  g_moveSending = false;

  if (!ok)
  {
    // The confirm itself failed - whatever was pending on the server is
    // already gone/invalid either way, so there's nothing left to cancel.
    exitMoveMode(false);
    g_lastModified = DetailSubDisplay::HP; // nothing left to show for the move
    goToError("move confirm failed: " + RelayClient::lastError(), AppState::CHARACTER_LIST);
    return;
  }

  exitMoveMode(false);                   // already committed server-side - nothing to cancel
  g_lastModified = DetailSubDisplay::HP; // the move's done - the shared sub-display falls back to showing HP
  g_justSent = true;
  g_sentBannerUntil = millis() + 1200;
  Display::showCharacterDetail(g_activeChar, g_addTarget, currentDisplayHp(), currentDisplayTempHp(), g_deltaValue, false, true,
                               currentMoveIndicator(), currentCircleIndicator());
}

static CircleIndicator currentCircleIndicator()
{
  CircleIndicator ci;
  ci.active = g_circleEditing;
  ci.drawn = g_circleDrawn;
  ci.radius = g_circleRadius;
  ci.units = g_moveUnits; // best-effort guess from the last move preview this session, if any - the relay
                          // doesn't tell us a scene's distance units any other way (see Types.h)
  ci.sending = g_circleSending;
  ci.enabled = g_settings.circleEnabled;
  return ci;
}

// Redraws the detail screen reflecting current state - whichever of
// HP-edit/move is the shared sub-display (g_lastModified) plus whatever
// circle state applies (editing/drawn) - so callers that just changed one
// piece of that don't each need to know the full parameter list.
static void redrawDetailScreen()
{
  Display::showCharacterDetail(g_activeChar, g_addTarget, currentDisplayHp(), currentDisplayTempHp(),
                               g_deltaValue, false, g_justSent, currentMoveIndicator(), currentCircleIndicator());
}

// Mirrors g_circleRadius into g_settings and writes settings.cfg, but only
// when it's actually different from what's already saved - avoids a
// pointless LittleFS write (and its flash-wear cost) on every visit where
// the radius was never touched from whatever it loaded as. Called
// whenever the radius is meant to be remembered as-is - on a genuine
// dial change, and on both ways of leaving radius-editing via Button B
// (see circleButtonShortPress()/circleButtonLongPress() below) - so the
// dialed-in value sticks across reboots and different characters. The
// one exception is dialing all the way down to zero with the encoder
// (handleCharacterDetail()'s zero-radius handling), which deliberately
// resets to the default instead of remembering zero.
static void persistCircleRadius()
{
  if (g_settings.circleRadius != g_circleRadius)
  {
    g_settings.circleRadius = g_circleRadius;
    Storage::saveSettings(g_settings);
  }
}

// Pushes the current g_circleRadius to the relay as a live preview (see
// RelayClient::previewCircle()) - called on entering radius-editing, on
// every radius change while it's open, and whenever a nudged move needs
// the (still-drawn) circle to re-center on the new target (see
// nudgeMove()). A failed preview call drops the circle entirely - both
// g_circleEditing and g_circleDrawn - rather than leaving the user dialing
// or believing in a radius that isn't actually being drawn anywhere
// anymore.
static void sendCirclePreview()
{
  g_circleSending = true;
  redrawDetailScreen();

  bool ok = RelayClient::previewCircle(g_activeChar.uuid, g_circleRadius);

  g_circleSending = false;

  if (!ok)
  {
    // The preview call itself is what failed - there's nothing pending on
    // the server for this attempt to clean up.
    g_circleEditing = false;
    g_circleDrawn = false;
    goToError("circle preview failed: " + RelayClient::lastError(), AppState::CHARACTER_LIST);
    return;
  }

  g_circleDrawn = true;
  redrawDetailScreen();
}

// Short press of Button B. Outside radius-editing, this is what opens it:
// hands the encoder over to the circle radius, without disturbing
// whichever of HP-edit/move is the current shared sub-display, and
// immediately previews at whatever radius is already dialed in (carried
// over from last time) rather than waiting for the first encoder turn.
// Once radius-editing is already open, a second short press instead
// removes whatever circle is currently drawn (cancelling the live
// preview) leaves radius-editing - the current
// radius is remembered (see persistCircleRadius()).
static void circleButtonShortPress()
{
  if (!g_circleEditing)
  {
    g_circleEditing = true;
    g_circleRadiusDirty = false;
    sendCirclePreview();
    return;
  }

  if (g_circleDrawn)
  {
    RelayClient::cancelCirclePreview(g_activeChar.uuid); // best-effort - NOT self-healing: the relay no longer expires this on its own, so a failure here leaves the preview showing until something else clears it
    g_circleDrawn = false;
  }
  g_circleEditing = false;
  g_circleRadiusDirty = false; // whatever hadn't been sent yet is moot - just cancelled the preview outright
  persistCircleRadius();
  redrawDetailScreen();
}

// Long press of Button B - the opposite of a second short press above:
// exits radius-editing (hands the encoder back to HP-edit/move) but
// leaves whatever circle is currently drawn exactly as it is, persisting
// on the map rather than being cancelled. Still remembers the radius
// (see persistCircleRadius()), same as the short-press case.
static void circleButtonLongPress()
{
  g_circleEditing = false;
  g_circleRadiusDirty = false;
  persistCircleRadius();
  redrawDetailScreen();
}

static void handleCharacterDetail()
{
  if (g_justSent && millis() > g_sentBannerUntil)
  {
    g_justSent = false;
    redrawDetailScreen();
  }

  // Poll every input exactly once per tick, regardless of mode - each
  // button/direction's debounce state machine (see InputManager.h) needs
  // to be called every loop() iteration to stay accurate, even on ticks
  // where a given result ends up unused below.
  ButtonEvent joyUp = InputManager::pollJoyUp();
  ButtonEvent joyDown = InputManager::pollJoyDown();
  ButtonEvent joyLeft = InputManager::pollJoyLeft();
  ButtonEvent joyRight = InputManager::pollJoyRight();
  ButtonEvent a = InputManager::pollButtonA();
  ButtonEvent b = InputManager::pollButtonB();

  // Press-and-hold auto-repeat for continuous movement - see
  // InputManager.h's pollJoy*Repeat() comment. Polled unconditionally
  // alongside the events above, same "every tick regardless of whether
  // it's used" requirement as the rest of this block.
  bool joyUpRepeat = InputManager::pollJoyUpRepeat();
  bool joyDownRepeat = InputManager::pollJoyDownRepeat();
  bool joyLeftRepeat = InputManager::pollJoyLeftRepeat();
  bool joyRightRepeat = InputManager::pollJoyRightRepeat();

  // Button B is global and independent of HP-edit/move. Outside
  // radius-editing, a short press opens it. Once it's open: a short press
  // instead removes whatever circle is currently drawn but stays in
  // radius-editing (see circleButtonShortPress()), while a long press
  // exits radius-editing but leaves the circle exactly as drawn (see
  // circleButtonLongPress()). Checked first so neither can also fall
  // through and register as anything else below in the same tick.
  //
  // Gated on g_settings.circleEnabled ("Circle Feature" on the settings
  // screen) - Button B is still polled every loop above regardless, but
  // its presses are dropped here when the setting is off.
  if (g_settings.circleEnabled)
  {
    if (b == ButtonEvent::LONG_PRESS)
    {
      circleButtonLongPress();
      return;
    }
    if (b == ButtonEvent::SHORT_PRESS)
    {
      circleButtonShortPress();
      return;
    }
  }


  // Encoder: dials the circle radius while radius-editing is open,
  // otherwise adjusts the pending HP delta. HP editing and a pending move
  // are always both "live" now - turning the encoder just switches which
  // one the shared sub-display below shows (g_lastModified), rather than
  // cancelling the other, so a move can be picked back up later by
  // nudging the joystick again right where it left off.
  if (g_circleEditing)
  {
    int32_t radiusDelta = InputManager::readEncoderDelta();
    if (radiusDelta != 0)
    {
      g_circleRadius += radiusDelta * CIRCLE_RADIUS_STEP;
      if (g_circleRadius < 0)
        g_circleRadius = 0; // dialing down this far removes the circle entirely - see the zero-radius handling just below
      if (g_circleRadius > CIRCLE_RADIUS_MAX)
        g_circleRadius = CIRCLE_RADIUS_MAX;

      if (g_circleRadius <= 0)
      {
        // Dialed all the way down to zero: remove the circle right away and
        // hand the encoder back (exit radius-editing) rather than waiting
        // for the send-debounce below - there's no reason to preview a
        // zero-radius circle first. The *remembered* radius resets to the
        // default here rather than staying at zero, so the next time
        // radius-editing is opened (Button B) it starts from something
        // visible again instead of needing a full dial-up from nothing.
        // This is deliberately different from a short press of Button B
        // (circleButtonShortPress()), which remembers whatever radius was
        // actually dialed in - dialing past zero here is really just
        // overshooting the bottom of the range, not an explicit request to
        // remember zero.
        g_circleRadiusDirty = false;
        g_circleEditing = false;
        if (g_circleDrawn)
        {
          RelayClient::cancelCirclePreview(g_activeChar.uuid); // best-effort, same caveat as elsewhere in this file
          g_circleDrawn = false;
        }
        g_circleRadius = CIRCLE_RADIUS_DEFAULT;
        persistCircleRadius();
        redrawDetailScreen();
        return;
      }

      // The on-screen number updates immediately; the (blocking, network
      // round-trip) live preview send is debounced below instead of firing
      // on every single tick - see CIRCLE_RADIUS_DEBOUNCE_MS.
      g_circleRadiusDirty = true;
      g_circleLastRadiusChangeMs = millis();
      redrawDetailScreen();
    }

    // Fires once, CIRCLE_RADIUS_DEBOUNCE_MS after the encoder last moved -
    // every tick in between just re-checks the elapsed time and does
    // nothing further, so a fast spin coalesces into a single preview send
    // once things settle rather than one per detent.
    if (g_circleRadiusDirty && millis() - g_circleLastRadiusChangeMs >= CIRCLE_RADIUS_DEBOUNCE_MS)
    {
      g_circleRadiusDirty = false;
      sendCirclePreview();
      if (g_state != AppState::CHARACTER_DETAIL)
        return; // sendCirclePreview() hit an error and already bailed to the error screen
    }
  
  if (a == ButtonEvent::LONG_PRESS)
  {
    circleButtonLongPress();
    return;
  }
  if (a == ButtonEvent::SHORT_PRESS)
  {
    circleButtonShortPress();
    return;
  }
  }
  else
  {
    int32_t delta = InputManager::readEncoderDelta();
    if (delta != 0)
    {
      g_dirty = true;
      g_lastModified = DetailSubDisplay::HP;
      g_deltaValue += delta;
      // Bounded by maxHp+tempHp rather than just maxHp, so a big enough swing
      // can still zero out HP through a full temp HP buffer in one edit (and
      // symmetrically, add that much fresh temp HP) instead of bottoming out
      // partway there.
      int maxSwingUp = g_activeChar.maxHp - g_activeChar.hp;
      if (g_addTarget == HpAddTarget::TEMP)
      {
        maxSwingUp = g_activeChar.maxHp - g_activeChar.tempHp;
      }
      int maxSwingDown = g_activeChar.hp + g_activeChar.tempHp;
      if (g_deltaValue < -maxSwingDown)
        g_deltaValue = -maxSwingDown;
      if (g_deltaValue > maxSwingUp)
        g_deltaValue = maxSwingUp;
      redrawDetailScreen();
    }
  }

  // Joystick: always accumulates onto the pending move and switches the
  // shared sub-display to show it (see nudgeMove()) - works the same
  // whether or not radius-editing is currently open, and never cancels
  // any pending HP delta. Direction presses are always instant, single
  // nudges - InputManager's pollJoy*() never classifies a direction as
  // LONG_PRESS/DOUBLE_PRESS or holds it back to check (see
  // InputManager.h), so a nonzero event here always means "just pressed,
  // right now." Each physical direction is remapped by
  // rotateForScreenRotation() before reaching nudgeMove(), so "up" always
  // nudges whatever's visually up on the (possibly rotated) screen.
  //
  // Gated on g_settings.moveButtonsEnabled ("Move Buttons" on the
  // settings screen) - still polled every loop above regardless (per
  // InputManager's polling contract) so debounce/repeat state stays
  // correct, but the presses themselves are simply dropped here when the
  // setting is off, same as if nothing were wired to the joystick.
  if (g_settings.moveButtonsEnabled)
  {
    if (joyUp != ButtonEvent::NONE || joyUpRepeat)
    {
      int dCol, dRow;
      rotateForScreenRotation(0, -1, dCol, dRow);
      nudgeMove(dCol, dRow);
      return;
    }
    if (joyDown != ButtonEvent::NONE || joyDownRepeat)
    {
      int dCol, dRow;
      rotateForScreenRotation(0, 1, dCol, dRow);
      nudgeMove(dCol, dRow);
      return;
    }
    if (joyLeft != ButtonEvent::NONE || joyLeftRepeat)
    {
      int dCol, dRow;
      rotateForScreenRotation(-1, 0, dCol, dRow);
      nudgeMove(dCol, dRow);
      return;
    }
    if (joyRight != ButtonEvent::NONE || joyRightRepeat)
    {
      int dCol, dRow;
      rotateForScreenRotation(1, 0, dCol, dRow);
      nudgeMove(dCol, dRow);
      return;
    }
  }

  // Button A acts on whichever of HP/move is currently the shared
  // sub-display (g_lastModified): a short press sends it. A long press
  // cancels whichever one is shown (if it actually has anything pending),
  // then swaps to showing the other one if *it* has something pending, or
  // heads back to the character list if neither does. (After a cancel, the
  // shared sub-display moves straight to the other one if it has something
  // pending, so each long press cancels exactly one thing.) A double press
  // always jumps to (and stays on) HP adjustment, even if move or circle
  // radius-editing was what's currently showing.
  if (a == ButtonEvent::SHORT_PRESS)
  {
    if (g_lastModified == DetailSubDisplay::MOVE && g_moveMode)
    {
      confirmMoveAndReturn();
    }
    else
    {
      sendCurrentHp();
    }
    return;
  }
  else if (a == ButtonEvent::LONG_PRESS)
  {
    // Whether the mode currently on screen actually has anything to give
    // up - an HP delta of 0 or no pending move both count as "nothing
    // here", in which case the cancel step below is a no-op and this just
    // falls through to checking the other side.
    bool currentHasPending = (g_lastModified == DetailSubDisplay::MOVE) ? g_moveMode : (g_deltaValue != 0);

    if (currentHasPending)
    {
      // There was something on the currently-shown sub-display to cancel:
      // clear it, but stay right here showing the same sub-display (now
      // empty) rather than falling through to the "nothing pending at
      // all" handling below - e.g. cancelling a queued move stays on the
      // move interface instead of bouncing back to the character list.
      if (g_lastModified == DetailSubDisplay::MOVE)
      {
        exitMoveMode(true); // cancel on server

        // If an HP edit is also pending, hand the shared sub-display over
        // to it right now, so the next long press cancels it directly
        // instead of first spending a press just swapping to it.
        if (g_deltaValue != 0)
          g_lastModified = DetailSubDisplay::HP;

        // While the move was pending, a drawn circle was following the
        // move target rather than the token's real position (see
        // nudgeMove()'s re-centering call). Cancelling the move snaps the
        // server-side token back to its actual position, so the circle
        // needs a fresh preview to snap back with it - otherwise it's
        // left drawn wherever the cancelled move had it pointing.
        if (g_circleDrawn)
        {
          sendCirclePreview();
          if (g_state != AppState::CHARACTER_DETAIL)
            return; // sendCirclePreview() hit an error and already bailed to the error screen
        }
      }
      else
      {
        g_deltaValue = 0;
        g_dirty = false; // nothing left pending here - safe for the background poll to refresh again

        // Mirror of the move case above: if a move is also still pending,
        // show it next so one more long press cancels it.
        if (g_moveMode)
          g_lastModified = DetailSubDisplay::MOVE;
      }
      redrawDetailScreen();
      return;
    }

    // Nothing was pending on the shown sub-display to begin with - check
    // whether the *other* one has something pending instead.
    bool otherHasPending = (g_lastModified == DetailSubDisplay::MOVE) ? (g_deltaValue != 0) : g_moveMode;

    if (otherHasPending)
    {
      g_lastModified = (g_lastModified == DetailSubDisplay::MOVE) ? DetailSubDisplay::HP : DetailSubDisplay::MOVE;
      redrawDetailScreen();
    }
    else
    {
          RelayClient::cancelCirclePreview(g_activeChar.uuid); // best-effort - NOT self-healing: the relay no longer expires this on its own, so a failure here leaves the preview showing until something else clears it
    g_circleDrawn = false;
      loadCharacterList(); // manual refresh, and returns to the character list
    }
    return;
  }
  else if (a == ButtonEvent::DOUBLE_PRESS)
  {
    // Toggle which pool a positive turn adds to. Subtracting is unaffected -
    // it always drains temp HP first, then HP, regardless of this setting.
    g_addTarget = (g_addTarget == HpAddTarget::NORMAL) ? HpAddTarget::TEMP : HpAddTarget::NORMAL;
    // Double-tapping A is also the explicit "give me HP adjustment" action -
    // hands the encoder back from radius-editing (if that was open) and
    // switches the shared sub-display to HP even if move was showing,
    // without cancelling either of them.
    g_circleEditing = false;
    g_lastModified = DetailSubDisplay::HP;
    redrawDetailScreen();
    return;
  }

  // Light background refresh of max HP / name (not current HP or temp HP,
  // so we never clobber an in-progress edit) while the user is looking at
  // this screen.
  if (!g_dirty && millis() - g_lastPollMs > CHARACTER_POLL_MS)
  {
    g_lastPollMs = millis();
    Character fresh;
    if (fetchCharacterByUuid(g_activeChar.uuid, fresh))
    {
      g_activeChar.name = fresh.name;
      g_activeChar.maxHp = fresh.maxHp;
      g_activeChar.tempHp = fresh.tempHp;
      g_activeChar.hp = fresh.hp;
      redrawDetailScreen();
    }
  }
}

static void handleErrorScreen()
{
  ButtonEvent a = InputManager::pollButtonA();
  ButtonEvent b = InputManager::pollButtonB();
  if (b == ButtonEvent::SHORT_PRESS)
  {
    if (g_stateAfterError == AppState::IP_ENTRY)
    {
      enterIpEntryDefaults();
    }
    else
    {
      loadCharacterList();
    }
  }

  else if (a == ButtonEvent::DOUBLE_PRESS)
  {
    if (g_stateAfterError == AppState::IP_ENTRY)
    {
      enterIpEntryDefaults();
    }
    else
    {
      loadCharacterList();
    }
  }

  else if (a == ButtonEvent::SHORT_PRESS)
  {
    if (g_stateAfterError == AppState::IP_ENTRY)
    {
      enterIpEntryDefaults();
    }
    else
    {
      loadCharacterList();
    }
  }
}

// ===========================================================================
// setup() / loop()
// ===========================================================================

void setup()
{
  Serial.begin(115200);
  delay(200);

  Display::begin();
  Storage::begin();

  // g_settings starts at AppSettings' defaults (Types.h); loadSettings()
  // only overwrites the fields it can actually parse out of settings.cfg,
  // so this is safe even on first boot with no file yet. Applied before
  // InputManager::begin() so its own debug print (if enabled) already
  // reflects the loaded setting rather than always using config.h's
  // compiled-in default.
  Storage::loadSettings(g_settings);
  Display::setRotation(g_settings.screenRotation);
  Display::setGaugeStyleFlat(g_settings.gaugeStyleFlat);
  InputManager::setDebugEnabled(g_settings.serialDebug);
  RelayClient::setTestMode(g_settings.relayTestMode);
  InputManager::setButtonsSwapped(g_settings.swapButtons);
  g_circleRadius = g_settings.circleRadius; // last radius used in circle mode, if any (see g_circleRadius' own comment)

  InputManager::begin();

  Display::showBoot("Starting WiFi...");
  delay(1000);
  // Tries every saved network first; opens the config portal only if none connect.
  bool wifiOk = connectWifi();

  if (!wifiOk)
  {
    goToError("WiFi setup timed out. Rebooting...", AppState::IP_ENTRY);
    delay(3000);
    ESP.restart();
  }

  Display::showBoot("WiFi connected");
  delay(1000);

  // Try the previously saved relay address first.
  String host;
  uint16_t port;
  if (Storage::loadRelayAddress(host, port))
  {
    g_state = AppState::RELAY_CONNECTING;
    if (tryConnectRelay(host, port))
    {
      loadCharacterList();
      return;
    }
  }

  // No saved address, or it didn't work: fall back to manual entry.
  enterIpEntryDefaults();
}

void loop()
{
  switch (g_state)
  {
  case AppState::IP_ENTRY:
    handleIpEntry();
    break;
  case AppState::CHARACTER_LIST:
    handleCharacterList();
    break;
  case AppState::CHARACTER_DETAIL:
    handleCharacterDetail();
    break;
  case AppState::SETTINGS:
    handleSettings();
    break;
  case AppState::WIFI_LIST:
    handleWifiList();
    break;
  case AppState::OTA_UPDATE:
    handleOta();
    break;
  case AppState::ERROR_SCREEN:
    handleErrorScreen();
    break;
  default:
    // BOOT / WIFI_CONNECTING / RELAY_CONNECTING / RELAY_TESTING_ENTRY /
    // LOADING_CHARACTERS are transient states driven synchronously from
    // setup() or the handlers above; nothing to poll here.
    break;
  }
  delay(5); // small yield; encoder is interrupt-driven so this doesn't cost responsiveness
}