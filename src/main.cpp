#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <WiFiManager.h>

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

// ===========================================================================
// Helpers
// ===========================================================================

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

static const SettingDescriptor SETTINGS_TABLE[] = {
    {"Screen rotation", settingApplyRotation, settingValueRotation},
    {"HP gauge style", settingApplyGaugeStyle, settingValueGaugeStyle},
    {"Serial Debug Output", settingApplySerialDebug, settingValueSerialDebug},
    {"Local Relay Test Mode", settingApplyRelayTestMode, settingValueRelayTestMode},
};
static const int SETTINGS_COUNT = sizeof(SETTINGS_TABLE) / sizeof(SETTINGS_TABLE[0]);

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
  if (b == ButtonEvent::LONG_PRESS)
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
  g_sending = true;
  int pendingHp = currentDisplayHp();
  int pendingTempHp = currentDisplayTempHp();
  Display::showCharacterDetail(g_activeChar, g_addTarget, pendingHp, pendingTempHp, g_deltaValue, true, false);

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
  }
  else
  {
    goToError("update failed: " + RelayClient::lastError(), AppState::CHARACTER_LIST);
    return;
  }
  Display::showCharacterDetail(g_activeChar, g_addTarget, pendingHp, pendingTempHp, g_deltaValue, false, g_justSent);
}

static void handleCharacterDetail()
{
  if (g_justSent && millis() > g_sentBannerUntil)
  {
    g_justSent = false;
    Display::showCharacterDetail(g_activeChar, g_addTarget, currentDisplayHp(), currentDisplayTempHp(), g_deltaValue, false, false);
  }

  int32_t delta = InputManager::readEncoderDelta();
  if (delta != 0)
  {
    g_dirty = true;
    g_deltaValue += delta;
    // Bounded by maxHp+tempHp rather than just maxHp, so a big enough swing
    // can still zero out HP through a full temp HP buffer in one edit (and
    // symmetrically, add that much fresh temp HP) instead of bottoming out
    // partway there.
    int maxSwing = g_activeChar.maxHp + g_activeChar.tempHp;
    if (g_deltaValue < -maxSwing)
      g_deltaValue = -maxSwing;
    if (g_deltaValue > maxSwing)
      g_deltaValue = maxSwing;
    Display::showCharacterDetail(g_activeChar, g_addTarget, currentDisplayHp(), currentDisplayTempHp(), g_deltaValue, false, false);
  }

  ButtonEvent a = InputManager::pollButtonA();
  if (a == ButtonEvent::SHORT_PRESS)
  {
    sendCurrentHp();
    return;
  }
  else if (a == ButtonEvent::LONG_PRESS)
  {
    loadCharacterList(); // manual refresh
    g_state = AppState::CHARACTER_LIST;
    Display::showCharacterList(g_characters, g_selectedIndex);
    g_lastPollMs = millis();
    return;
  }

else if (a == ButtonEvent::DOUBLE_PRESS)
  {
// Toggle which pool a positive turn adds to. Subtracting is unaffected -
    // it always drains temp HP first, then HP, regardless of this setting.
    g_addTarget = (g_addTarget == HpAddTarget::NORMAL) ? HpAddTarget::TEMP : HpAddTarget::NORMAL;
    Display::showCharacterDetail(g_activeChar, g_addTarget, currentDisplayHp(), currentDisplayTempHp(), g_deltaValue, false, false);
  return;
  }


  ButtonEvent b = InputManager::pollButtonB();
  if (b == ButtonEvent::SHORT_PRESS)
  {
    // Toggle which pool a positive turn adds to. Subtracting is unaffected -
    // it always drains temp HP first, then HP, regardless of this setting.
    g_addTarget = (g_addTarget == HpAddTarget::NORMAL) ? HpAddTarget::TEMP : HpAddTarget::NORMAL;
    Display::showCharacterDetail(g_activeChar, g_addTarget, currentDisplayHp(), currentDisplayTempHp(), g_deltaValue, false, false);
  return;
  }

  else if (b == ButtonEvent::LONG_PRESS)
  {
    // Manual refresh of just this character (pulls latest server-side HP,
    // e.g. if someone else changed it in Foundry since we loaded it).
    // Character fresh;
    // if (fetchCharacterByUuid(g_activeChar.uuid, fresh)) {
    //   g_activeChar = fresh;
    //   if (!g_dirty) {
    //     g_deltaValue = 0;
    //   }
    //  Display::showCharacterDetail(g_activeChar, g_addTarget, currentDisplayHp(), currentDisplayTempHp(), g_deltaValue, false, false);
    // return;
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
      Display::showCharacterDetail(g_activeChar, g_addTarget, currentDisplayHp(), currentDisplayTempHp(), g_deltaValue, false, false);
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

  InputManager::begin();

  Display::showBoot("Starting WiFi...");

  Display::showWifiPortalHint();
  WiFiManager wm;
  wm.setConfigPortalTimeout(WIFI_PORTAL_TIMEOUT_S);
  // Blocks until connected, portal times out, or user configures WiFi.
  bool wifiOk = wm.autoConnect("FoundryHPMonitor-Setup");

  if (!wifiOk)
  {
    goToError("WiFi setup timed out. Rebooting...", AppState::IP_ENTRY);
    delay(3000);
    ESP.restart();
  }

  Display::showBoot("WiFi connected");
  delay(400);

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
