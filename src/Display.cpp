#include "Display.h"
#include "config.h"
#include "InputManager.h"
#include "char_o_matic_logo.h"
#include <TFT_eSPI.h>
#include <math.h>

// NOTE ON ESP8266 RAM: the ESP32 version of this file used an offscreen
// 240x240 sprite (~115KB) to double-buffer the character-detail screen for
// flicker-free updates while turning the encoder. That doesn't fit in the
// ESP8266's usable heap (~30-45KB free with WiFi connected), so this
// version draws straight to the display instead. To keep flicker to a
// minimum without a framebuffer, showCharacterDetail() never blanks the
// whole screen on an update - it only repaints the exact regions that
// changed (the ring arc, and fixed-width text rows), which in practice
// looks close to flicker-free since nothing ever goes through a visible
// black "clear" step.

namespace Display
{

  static TFT_eSPI tft = TFT_eSPI();

  // Tracks which character showCharacterDetail() last fully painted, so it
  // knows whether to do a full repaint (see freshCharacter there) or just an
  // incremental one. File-scope rather than local to showCharacterDetail so
  // showCharacterList() can invalidate it below - otherwise, going
  // select -> detail -> back to select -> detail on the *same* character
  // again would look unchanged to showCharacterDetail() and skip the full
  // repaint, leaving the select screen's graphics showing through around the
  // ring and text.
  static String s_lastDetailUuid;

  // Tracks what showCharacterList() last actually painted, so a call with
  // identical data (e.g. a background poll that came back unchanged) can be
  // skipped entirely instead of doing a fillScreen()+full repaint that would
  // just draw the exact same pixels back - that fillScreen is what causes
  // the visible flicker. s_listValid is cleared by every *other* full-screen
  // draw function below so that switching back to the list screen from
  // somewhere else always forces a real repaint, even if the list content
  // and selection happen to match what was last drawn before we left it.
  static bool s_listValid = false;
  static std::vector<Character> s_lastListChars;
  static int s_lastListSelectedIndex = -1;

  // Runtime-editable settings, applied via setRotation()/setGaugeStyleFlat()
  // below. Defaulted here to match AppSettings' own defaults (Types.h) /
  // the old hardcoded tft.setRotation(2) and compile-time GAUGE_STYLE_FLAT,
  // so the display looks the same as before until main.cpp loads (or the
  // user edits) actual settings.
  static uint8_t s_currentRotation = 2;
  static bool s_gaugeStyleFlat = true;

  // BUTTON_A_LABEL/BUTTON_B_LABEL (config.h) name the two physical
  // buttons; labelA()/labelB() below pick whichever of the two currently
  // performs a given logical role, per InputManager::buttonsSwapped() -
  // see AppSettings::swapButtons' comment (Types.h) for why the swap
  // itself lives there rather than here. Every on-screen button hint
  // (e.g. ">A<:next") is built through these four helpers rather than
  // ever spelling out "A"/"B" inline, so both the label characters
  // themselves (config.h) and the current swap state stay in one place.
  static char labelA() { return InputManager::buttonsSwapped() ? BUTTON_B_LABEL : BUTTON_A_LABEL; }
  static char labelB() { return InputManager::buttonsSwapped() ? BUTTON_A_LABEL : BUTTON_B_LABEL; }
  static String shortLabel(char c) { return ">" + String(c) + "<"; }
  static String longLabel(char c) { return "[" + String(c) + "]"; }
  static String doubleLabel(char c) { return ">" + String(c) + String(c) + "<"; }

  static bool sameCharacterData(const Character &a, const Character &b)
  {
    return a.uuid == b.uuid && a.name == b.name && a.hp == b.hp &&
           a.maxHp == b.maxHp && a.tempHp == b.tempHp;
  }

  static bool sameCharacterList(const std::vector<Character> &a, const std::vector<Character> &b)
  {
    if (a.size() != b.size())
      return false;
    for (size_t i = 0; i < a.size(); i++)
    {
      if (!sameCharacterData(a[i], b[i]))
        return false;
    }
    return true;
  }

#define COL_BG TFT_BLACK
#define COL_TEXT TFT_WHITE
#define COL_DIM 0x4A49   // dark grey
#define COL_TRACK 0x2965 // ring background track
#define COL_ACCENT TFT_CYAN
#define COL_HP_GOOD TFT_GREEN
#define COL_HP_WARN TFT_YELLOW
#define COL_HP_BAD TFT_RED
#define COL_TEMP 0x051F   // light blue for temp hp
#define COL_TEMP_LINE 0x0290 // darker blue (about half brightness) for the thin pending-change line on the temp HP ring, so it reads as clearly different from the bright fill
#define COL_CIRCLE 0xA29F // light purple for the circle-radius readout and circle mode - distinct from temp hp's blue and the cyan accent
#define COL_ELEV 0xFBE0   // warm orange for the token elevation readout and elevation mode - distinct from the blue/cyan/purple above

// HP ring's inner radius (see showCharacterDetail()'s hpInnerR, which this
// must be kept in sync with) - hoisted up here so drawTextRow() callers
// below can compute how much horizontal room a row has before it runs
// into the ring's visible band, without drawTextRow() itself needing to
// know anything about the ring.
static const int RING_INNER_R = 98;

// The half-width (in px, centered on SCREEN_CX) of the HP/temp ring's
// empty inner hole at a given vertical distance `absDy` from SCREEN_CY -
// i.e. the widest a row can safely blank/draw at that height before its
// left/right edges start eating into the ring's visible band instead of
// just the empty background inside it. Returns 0 once absDy reaches the
// ring's inner radius (no hole left at all at that height).
static int ringHoleHalfWidth(int absDy)
{
  if (absDy >= RING_INNER_R)
    return 0;
  return (int)sqrtf((float)(RING_INNER_R * RING_INNER_R - absDy * absDy));
}

// Max characters per line when the character name is wrapped onto the two
// name lines on the detail screen (see splitNameLines()). Override in
// config.h if a project wants longer/shorter lines for a different font
// size or panel.
#ifndef CHAR_NAME_LINE_MAX_LEN
#define CHAR_NAME_LINE_MAX_LEN 10
#endif

// Same idea, for showCharacterList()'s unselected carousel rows, which are
// drawn at half the point size of the detail screen's name (size 1 vs
// size 2) - roughly twice as many characters fit in the same width, so
// this defaults higher. Override in config.h alongside the one above.
#ifndef CHAR_NAME_LIST_LINE_MAX_LEN
#define CHAR_NAME_LIST_LINE_MAX_LEN 14
#endif

// Whether the HP ring is a solid red/yellow/green band color (1) or a
// smooth red->yellow->green gradient around the whole circumference (0).
// Override in config.h.
#ifndef GAUGE_STYLE_FLAT
#define GAUGE_STYLE_FLAT 0
#endif

  // If the name contains a quoted nickname (e.g. Aragorn "Strider" son of
  // Arathorn), show only that part - it's usually shorter and more
  // identifying than the full name, and the parenthetical/legal parts around
  // it aren't useful on a tiny round display. Falls back to the full name if
  // there's no quoted section.
  static String extractDisplayName(const String &name)
  {
    int firstQuote = name.indexOf('"');
    if (firstQuote >= 0)
    {
      int secondQuote = name.indexOf('"', firstQuote + 1);
      if (secondQuote > firstQuote)
      {
        String inner = name.substring(firstQuote + 1, secondQuote);
        inner.trim();
        if (inner.length() > 0)
          return inner;
      }
    }
    return name;
  }

  // Truncates s to maxLen characters, replacing the last two with ".." when
  // truncation actually happens so it's visually distinguishable from a name
  // that just happens to end there.
  static String truncateLine(const String &s, int maxLen)
  {
    if ((int)s.length() <= maxLen)
      return s;
    if (maxLen <= 2)
      return s.substring(0, maxLen);
    return s.substring(0, maxLen - 2) + "..";
  }

  // Splits a character's display name across (up to) two lines, each at most
  // maxLineLen characters. Used both by showCharacterList()'s carousel rows
  // and showCharacterDetail()'s name rows. Prefers breaking at the space
  // closest to the name's midpoint that still leaves both halves within
  // maxLineLen, so e.g. "Sir Reginald Fluffington" breaks into two
  // reasonably balanced lines rather than greedily stuffing line 1. If no
  // such space exists (single very long word, or the name just doesn't fit
  // in two lines at this length), hard-wraps at maxLineLen and truncates
  // each line independently as a last resort.
  static void splitNameLines(const String &fullName, int maxLineLen, String &line1, String &line2)
  {
    String name = extractDisplayName(fullName);
    name.trim();
    if (maxLineLen < 1)
      maxLineLen = 1;

    if ((int)name.length() <= maxLineLen)
    {
      line1 = name;
      line2 = "";
      return;
    }

    int bestSpace = -1;
    long bestDist = 1000000;
    int mid = name.length() / 2;
    for (int i = 0; i < (int)name.length(); i++)
    {
      if (name.charAt(i) != ' ')
        continue;
      int len1 = i;
      int len2 = (int)name.length() - i - 1;
      if (len1 > maxLineLen || len2 > maxLineLen)
        continue;
      long dist = labs((long)i - (long)mid);
      if (dist < bestDist)
      {
        bestDist = dist;
        bestSpace = i;
      }
    }

    if (bestSpace >= 0)
    {
      line1 = name.substring(0, bestSpace);
      line2 = name.substring(bestSpace + 1);
      return;
    }

    line1 = truncateLine(name.substring(0, min((int)name.length(), maxLineLen)), maxLineLen);
    line2 = ((int)name.length() > maxLineLen) ? truncateLine(name.substring(maxLineLen), maxLineLen) : "";
  }

  void begin()
  {
    tft.init();
    tft.setRotation(s_currentRotation); // 180 degrees by default; see setRotation() to change at runtime
    tft.fillScreen(COL_BG);
    if (PIN_TFT_BL >= 0)
    {
      pinMode(PIN_TFT_BL, OUTPUT);
      digitalWrite(PIN_TFT_BL, HIGH);
    }
    // PIN_TFT_BL is -1 by default for this board (backlight wired straight
    // to 3V3), in which case there's nothing to do here.
  }

  void setRotation(uint8_t rotation)
  {
    if (rotation > 3)
      rotation = 3;
    if (rotation == s_currentRotation)
      return;
    s_currentRotation = rotation;
    tft.setRotation(rotation);
    tft.fillScreen(COL_BG);

    // Every "what did we last draw" cache below assumes the geometry it
    // was drawn under is still current - force a full repaint of whichever
    // screen gets shown next rather than leaving stale content on screen or
    // letting an unchanged-data check skip a needed repaint.
    s_listValid = false;
    s_lastDetailUuid = "";
  }

  void setGaugeStyleFlat(bool flat)
  {
    s_gaugeStyleFlat = flat;
  }

  static void centeredText(TFT_eSPI &d, const String &s, int y, uint8_t size, uint16_t color)
  {
    d.setTextColor(color, COL_BG);
    d.setTextSize(size);
    d.setTextDatum(MC_DATUM);
    d.drawString(s, SCREEN_CX, y);
  }

  void showBoot(const String &line)
  {
    s_listValid = false;
    //tft.fillScreen(COL_BG);
    //centeredText(tft, "Foundry Controller", 95, 1, COL_ACCENT);
    
    showLogo();
    centeredText(tft, line, 180, 1, COL_TEXT);
  }

  void showLogo()
  {
    s_listValid = false;
    tft.fillScreen(COL_BG);
    // The RGB565 bitmap is opaque (baked-in black background, not a
    // transparent mask), so it just needs to land in the right spot -
    // no separate clear step beyond the fillScreen above.
    //
    // The bitmap is stored run-length compressed in flash (see
    // char_o_matic_logo.h), so decode it a row at a time into a small stack
    // buffer and push each row from there - one row (162 * 2 = 324 bytes
    // here) is a trivial amount of stack, and the decoder only does byte
    // reads from PROGMEM (pgm_read_byte), which are safe on the ESP8266
    // without any alignment concerns.
    int x = (tft.width() - CHAR_O_MATIC_LOGO_W) / 2;
    int y = (tft.height() - CHAR_O_MATIC_LOGO_H) / 2 - 40;
    uint16_t rowBuf[CHAR_O_MATIC_LOGO_W];
    CharOMaticLogoDecoder logo;
    charOMaticLogoBegin(logo);
    for (int row = 0; row < CHAR_O_MATIC_LOGO_H; row++)
    {
      charOMaticLogoRead(logo, rowBuf, CHAR_O_MATIC_LOGO_W);
      tft.pushImage(x, y + row, CHAR_O_MATIC_LOGO_W, 1, rowBuf);
    }

    // Firmware version, small and dim, centered just under the wordmark
    // (the boot status line from showBoot() sits further down, at y=180).
    centeredText(tft, String("v") + FIRMWARE_VERSION, y + CHAR_O_MATIC_LOGO_H + 12, 1, COL_DIM);
  }

  void showWifiPortalHint()
  {
    s_listValid = false;
    tft.fillScreen(COL_BG);
    centeredText(tft, "WiFi setup needed", 70, 1, COL_ACCENT);
    centeredText(tft, "Connect phone/PC to:", 105, 1, COL_TEXT);
    centeredText(tft, "FoundryController-Setup", 125, 1, COL_TEXT);
    centeredText(tft, "then follow the portal", 150, 1, COL_DIM);
    centeredText(tft, "or go to 192.168.4.1", 165, 1, COL_DIM);
    centeredText(tft, "to pick your WiFi.", 180, 1, COL_DIM);
  }

  void showRelayConnecting(const String &host, uint16_t port)
  {
    s_listValid = false;
    tft.fillScreen(COL_BG);
    centeredText(tft, "Connecting to relay", 100, 1, COL_ACCENT);
    centeredText(tft, host + ":" + String(port), 130, 1, COL_TEXT);
  }

  void showError(const String &message)
  {
    s_listValid = false;
    tft.fillScreen(COL_BG);
    centeredText(tft, "Error", 80, 2, COL_HP_BAD);
    centeredText(tft, message, 120, 1, COL_TEXT);
    if (InputManager::debugEnabled())
    {
      Serial.println(message);
    }
    centeredText(tft, "Press any button to go back", 160, 1, COL_DIM);
  }

  void showOtaStatus(const String &line1, const String &line2,
                     const String &title, const String &footer)
  {
    s_listValid = false;
    tft.fillScreen(COL_BG);
    centeredText(tft, title, 70, 1, COL_ACCENT);
    centeredText(tft, String("v") + FIRMWARE_VERSION, 84, 1, COL_DIM);
    centeredText(tft, line1, 105, 1, COL_TEXT);
    centeredText(tft, line2, 125, 1, COL_TEXT);
    centeredText(tft, footer, 190, 1, COL_DIM);
  }

  void showOtaProgress(int percent)
  {
    percent = constrain(percent, 0, 100);
    const int x = 40, y = 145, w = 160, h = 12;
    tft.fillRect(x, y, w, h, COL_TRACK);
    tft.fillRect(x, y, (w * percent) / 100, h, COL_HP_GOOD);
    tft.fillRect(x, y + 20, w, 10, COL_BG); // clear the previous number
    centeredText(tft, String(percent) + "%", y + 25, 1, COL_TEXT);
  }

  void showLoadingCharacters()
  {
    s_listValid = false;
    tft.fillScreen(COL_BG);
    centeredText(tft, "Loading characters...", 120, 1, COL_ACCENT);
  }

  void showIpEntry(const uint16_t fields[5], int fieldIndex, bool fakery)
  {
    s_listValid = false;
    tft.fillScreen(COL_BG);
    centeredText(tft, "Enter relay address", 45, 1, COL_ACCENT);

    String portStr = String(fields[4]);

    // Draw IP with the active field highlighted
    tft.setTextSize(2);
    tft.setTextDatum(MC_DATUM);

    String parts[4] = {String(fields[0]), String(fields[1]), String(fields[2]), String(fields[3])};

    // simple fixed layout: draw each octet centered in its own slot
    int slotX[4] = {45, 95, 145, 195};
    for (int i = 0; i < 4; i++)
    {
      uint16_t color = (i == fieldIndex) ? COL_ACCENT : COL_TEXT;
      tft.setTextColor(color, COL_BG);
      tft.drawString(parts[i], slotX[i], 110);
      if (i < 3)
      {
        tft.setTextColor(COL_DIM, COL_BG);
        tft.drawString(".", slotX[i] + 22, 110);
      }
    }

    uint16_t portColor = (fieldIndex == 4) ? COL_ACCENT : COL_TEXT;
    tft.setTextSize(1);
    tft.setTextColor(COL_DIM, COL_BG);
    tft.drawString("port", SCREEN_CX, 135);
    tft.setTextSize(2);
    tft.setTextColor(portColor, COL_BG);
    tft.drawString(portStr, SCREEN_CX, 158);

    tft.setTextSize(1);
    tft.setTextColor(COL_DIM, COL_BG);
    tft.drawString(shortLabel(labelA()) + ":next " + longLabel(labelA()) + ":go", SCREEN_CX, 200);
    tft.drawString(longLabel(labelB()) + ":settings", SCREEN_CX, 215);

    if (fakery)
    {
      tft.setTextSize(2);
      tft.setTextColor(COL_HP_BAD, COL_BG);
      tft.drawString("LOCAL TEST MODE ON", SCREEN_CX, 80);
    }
  }

  void showOtaSendTarget(const uint16_t octets[4], int fieldIndex)
  {
    s_listValid = false;
    s_lastListSelectedIndex = -1;
    s_lastDetailUuid = "";
    tft.fillScreen(COL_BG);
    centeredText(tft, "Send firmware to", 45, 1, COL_ACCENT);
    centeredText(tft, String("this unit: v") + FIRMWARE_VERSION, 68, 1, COL_DIM);

    tft.setTextSize(2);
    tft.setTextDatum(MC_DATUM);
    int slotX[4] = {45, 95, 145, 195};
    for (int i = 0; i < 4; i++)
    {
      tft.setTextColor((i == fieldIndex) ? COL_ACCENT : COL_TEXT, COL_BG);
      tft.drawString(String(octets[i]), slotX[i], 110);
      if (i < 3)
      {
        tft.setTextColor(COL_DIM, COL_BG);
        tft.drawString(".", slotX[i] + 22, 110);
      }
    }

    tft.setTextSize(1);
    tft.setTextColor(COL_DIM, COL_BG);
    tft.drawString("Target must be on its", SCREEN_CX, 145);
    tft.drawString("Firmware Update screen", SCREEN_CX, 158);
    tft.drawString(shortLabel(labelA()) + ":next " + longLabel(labelA()) + ":send", SCREEN_CX, 200);
    tft.drawString(longLabel(labelB()) + ":back", SCREEN_CX, 215);
  }

  void showSettings(const std::vector<String> &names, const std::vector<String> &values, int selectedIndex)
  {
    s_listValid = false;
    // Whatever's cached for the character list/detail screens no longer
    // reflects what's on screen now - force a full repaint of either the
    // next time we get back to them.
    s_lastListSelectedIndex = -1;
    s_lastDetailUuid = "";

    tft.fillScreen(COL_BG);
    centeredText(tft, "Settings", 30, 1, COL_ACCENT);

    int n = names.size();
    if (n == 0)
    {
      centeredText(tft, "Nothing to configure", 110, 1, COL_TEXT);
      return;
    }
    if (selectedIndex < 0)
      selectedIndex = 0;
    if (selectedIndex >= n)
      selectedIndex = n - 1;

    // Same windowed carousel look as showCharacterList(): up to 3 rows
    // centered on the selection, each row here being a setting name over
    // its current value rather than a character name over its HP.
    int rowH = 48;
    int centerY = 110;

    for (int offset = -1; offset <= 1; offset++)
    {
      int idx = selectedIndex + offset;
      if (idx < 0 || idx >= n)
        continue;

      int y = centerY + offset * rowH;
      bool isSel = (offset == 0);

      tft.setTextDatum(MC_DATUM);

      tft.setTextSize(1);
      tft.setTextColor(COL_DIM, COL_BG);
      tft.drawString(names[idx], SCREEN_CX, y - (isSel ? 12 : 8));

      uint16_t valColor = isSel ? COL_ACCENT : COL_DIM;
      uint8_t valSize = isSel ? 2 : 1;
      tft.setTextSize(valSize);
      tft.setTextColor(valColor, COL_BG);
      tft.drawString(values[idx], SCREEN_CX, y + (isSel ? 8 : 6));
    }

    tft.setTextSize(1);
    tft.setTextColor(COL_DIM, COL_BG);
    tft.setTextDatum(MC_DATUM);
    tft.drawString(shortLabel(labelA()) + ":next", SCREEN_CX, 200);
    tft.drawString(longLabel(labelA()) + ":save " + longLabel(labelB()) + ":back", SCREEN_CX, 215);
  }

  void showWifiList(const std::vector<String> &ssids, const std::vector<String> &notes,
                    int selectedIndex, bool confirmDelete)
  {
    s_listValid = false;
    s_lastListSelectedIndex = -1;
    s_lastDetailUuid = "";

    tft.fillScreen(COL_BG);
    centeredText(tft, "Saved WiFi", 30, 1, COL_ACCENT);

    int n = ssids.size();
    if (n == 0)
    {
      centeredText(tft, "No saved networks", 110, 1, COL_TEXT);
    }
    else
    {
      if (selectedIndex < 0)
        selectedIndex = 0;
      if (selectedIndex >= n)
        selectedIndex = n - 1;

      int rowH = 48;
      int centerY = 110;
      for (int offset = -1; offset <= 1; offset++)
      {
        int idx = selectedIndex + offset;
        if (idx < 0 || idx >= n)
          continue;

        int y = centerY + offset * rowH;
        bool isSel = (offset == 0);

        tft.setTextDatum(MC_DATUM);
        // SSIDs can be up to 32 chars - only the selected row gets the big
        // font, and only when the name is short enough to fit.
        uint8_t size = (isSel && ssids[idx].length() <= 15) ? 2 : 1;
        tft.setTextSize(size);
        tft.setTextColor(isSel ? COL_ACCENT : COL_DIM, COL_BG);
        tft.drawString(ssids[idx], SCREEN_CX, y - (isSel ? 6 : 0));

        if (isSel)
        {
          tft.setTextSize(1);
          if (confirmDelete)
          {
            tft.setTextColor(COL_HP_BAD, COL_BG);
            tft.drawString("Press again to delete", SCREEN_CX, y + 18);
          }
          else if (notes[idx].length() > 0)
          {
            tft.setTextColor(COL_HP_GOOD, COL_BG);
            tft.drawString(notes[idx], SCREEN_CX, y + 18);
          }
        }
      }
    }

    tft.setTextSize(1);
    tft.setTextColor(COL_DIM, COL_BG);
    tft.setTextDatum(MC_DATUM);
    if (n > 0)
      tft.drawString(shortLabel(labelA()) + ":next " + shortLabel(labelB()) + ":delete", SCREEN_CX, 200);
    tft.drawString(longLabel(labelA()) + "/" + longLabel(labelB()) + ":back", SCREEN_CX, 215);
  }

  void showCharacterList(const std::vector<Character> &chars, int selectedIndex)
  {
    // Nothing actually changed since the last time this screen was painted
    // (common on a background poll that came back with the same data) - skip
    // the fillScreen()+repaint entirely rather than redrawing identical
    // pixels, which is what was causing the visible flicker.
    if (s_listValid && selectedIndex == s_lastListSelectedIndex && sameCharacterList(chars, s_lastListChars))
    {
      return;
    }

    tft.fillScreen(COL_BG);

    // Force showCharacterDetail()'s next call to do a full repaint, even if
    // it ends up being the same character as before - see s_lastDetailUuid.
    s_lastDetailUuid = "";

    if (chars.empty())
    {
      centeredText(tft, "No player characters", 110, 1, COL_TEXT);
      centeredText(tft, "found in session", 130, 1, COL_TEXT);
      s_listValid = true;
      s_lastListChars = chars;
      s_lastListSelectedIndex = selectedIndex;
      return;
    }

    centeredText(tft, "Select character", 30, 1, COL_ACCENT);

    // Show up to 3 rows centered around the selection, simple carousel style.
    int rowH = 48;     // prev 42
    int centerY = 110; // prev 120
    int n = chars.size();

    for (int offset = -1; offset <= 1; offset++)
    {
      int idx = selectedIndex + offset;
      if (idx < 0 || idx >= n)
        continue;

      int y = centerY + offset * rowH;
      bool isSel = (offset == 0);
      uint16_t nameColor = isSel ? COL_ACCENT : COL_DIM;
      uint8_t size = isSel ? 2 : 1;

      const Character &c = chars[idx];
      String nameLine1, nameLine2;
      int maxLen = isSel ? CHAR_NAME_LINE_MAX_LEN : CHAR_NAME_LIST_LINE_MAX_LEN;
      splitNameLines(c.name, maxLen, nameLine1, nameLine2);
      bool twoLines = nameLine2.length() > 0;

      tft.setTextSize(size);
      tft.setTextColor(nameColor, COL_BG);
      tft.setTextDatum(MC_DATUM);

      // Names that fit on one line keep the original single-line position
      // (y) so short names look exactly as before; names that need to wrap
      // split around it instead. The selected row gets a wider split (and
      // pushes its HP line down to compensate) since its text is drawn a
      // size larger than the neighbor rows.
      int nameY1 = twoLines ? y - (isSel ? 14 : 6) : y;
      int nameY2 = y + (isSel ? 2 : 6);

      tft.drawString(nameLine1, SCREEN_CX, nameY1);
      if (twoLines)
        tft.drawString(nameLine2, SCREEN_CX, nameY2);

      if (isSel)
      {
        String hpLine = String(c.hp) + "/" + String(c.maxHp) + " HP";
        if (c.tempHp > 0)
          hpLine += " (+" + String(c.tempHp) + ")";
        tft.setTextSize(1);
        tft.setTextColor(COL_TEXT, COL_BG);
        tft.drawString(hpLine, SCREEN_CX, y + (twoLines ? 18 : 16));
      }
    }

    tft.setTextSize(1);
    tft.setTextColor(COL_DIM, COL_BG);
    tft.setTextDatum(MC_DATUM);
    tft.drawString(shortLabel(labelA()) + ": select", SCREEN_CX, 200);
    tft.drawString(longLabel(labelB()) + ": IP input", SCREEN_CX, 215);

    s_listValid = true;
    s_lastListChars = chars;
    s_lastListSelectedIndex = selectedIndex;
  }

  // TFT_eSPI's native drawArc() measures its angles clockwise from the 6
  // o'clock position; the rest of this file thinks in degrees clockwise from
  // the top (12 o'clock), so this just re-bases one into the other.
  static inline uint32_t toTftAngle(float topClockwiseDeg)
  {
    float a = topClockwiseDeg + 180.0f;
    if (a >= 360.0f)
      a -= 360.0f;
    if (a < 0.0f)
      a += 360.0f;               // guard, shouldn't happen given callers, but cheap insurance
    return (uint32_t)(a + 0.5f); // round to nearest, not truncate
  }

  // Draws a ring segment from startDeg to endDeg (0deg = top, clockwise),
  // between outerR and innerR, by delegating to TFT_eSPI's native drawArc().
  // That gives properly anti-aliased curved edges (the old approach here
  // stamped overlapping filled circles along the arc path, which only looked
  // smooth when the ring was thick enough for consecutive stamps to overlap -
  // a thin ring like the temp HP one ended up looking like a dotted line).
  // Square, non-anti-aliased ends are intentional: that's what plain drawArc
  // gives you (vs. drawSmoothArc), and it's what lets consecutive calls
  // grow/shrink a ring one segment at a time without leaving stray
  // anti-aliased seams at the joins between old and new segments.

  static void drawGaugeArc(TFT_eSPI &d, int cx, int cy, int outerR, int innerR,
                           float startDeg, float endDeg, uint16_t color, bool smooth = true)
  {
    if (endDeg <= startDeg)
      return;
    if (endDeg - startDeg >= 360.0f)
    {
      // A full lap is rotation-independent - don't route it through
      // toTftAngle(), which collapses 0 and 360 to the same raw angle and
      // would draw a zero-length (i.e. invisible) arc here instead of a
      // full circle.
      d.drawArc(cx, cy, outerR, innerR, 0, 360, color, COL_BG, smooth);
      return;
    }
    d.drawArc(cx, cy, outerR, innerR, toTftAngle(startDeg), toTftAngle(endDeg), color, COL_BG, smooth);
  }

  // Returns an interpolated red->orange->yellow->green color for a gauge
  // position, where frac 0.0 is the empty end (red) and 1.0 is the full end
  // (green); orange sits at 0.5 (see the remap at the top of the function).
  static uint16_t gaugeGradientColor(float frac)
  {
    if (frac < 0.0f)
      frac = 0.0f;
    if (frac > 1.0f)
      frac = 1.0f;

    // Shift the gradient so the color that used to mean 25% HP remaining
    // (orange) now means 50%: 0-50% sweeps red -> orange, 50-100% sweeps
    // orange -> yellow -> green (the original curve's 0-25% and 25-100%
    // stretches). Continuous at 50%. Applies everywhere this is used - the
    // ring in both flat and gradient styles, and the HP number's color.
    if (frac < 0.5f)
      frac = frac * 0.5f;
    else
      frac = 0.25f + (frac - 0.5f) * 1.5f;
    uint8_t r, g;
    const uint8_t b = 0;
    if (frac < 0.5f)
    {
      r = 255;
      g = (uint8_t)(255.0f * (frac / 0.5f)); // red -> yellow
    }
    else
    {
      r = (uint8_t)(255.0f * (1.0f - (frac - 0.5f) / 0.5f)); // yellow -> green
      g = 255;
    }
    return tft.color565(r, g, b);
  }

// Degrees per gradient wedge when painting/erasing the HP ring. Smaller =
// smoother gradient but more drawArc() calls (each has its own overhead
// on the ESP8266). 4 degrees = up to 90 wedges for a full-circle repaint
// (character switch); incremental updates are usually far smaller than that.
#define GAUGE_WEDGE_DEG 4.0f

  // Paints the HP ring across [fromDeg, toDeg), coloring each small wedge
  // along a fixed red->yellow->green gradient keyed to its own angular
  // position (angle/360), rather than one flat color for the whole arc.
  static void paintGaugeGradient(TFT_eSPI &d, int cx, int cy, int outerR, int innerR,
                                 float fromDeg, float toDeg)
  {
    if (toDeg <= fromDeg)
      return;
    float a = fromDeg;
    while (a < toDeg)
    {
      float next = min(a + GAUGE_WEDGE_DEG, toDeg);
      float mid = (a + next) * 0.5f;
      drawGaugeArc(d, cx, cy, outerR, innerR, a, next, gaugeGradientColor(mid / 360.0f));
      a = next;
    }
  }

  // -----------------------------------------------------------------------
  // HP / temp HP ring drawing with a "pending change" indicator.
  //
  // While HP (or temp HP) is being edited, the ring keeps showing the
  // committed value at full width, and the change itself is drawn as a thin
  // line along the arc between the committed and the pending value, with a
  // full-width tick at the pending end (the tick, the full-width fill and the
  // thin line are all drawn in the color of the pending health level):
  //   - healing:  full-width fill up to the committed value, then the thin
  //               line out to the pending value, tick at its far end
  //   - damage:   full-width fill up to the pending value, tick there, then
  //               the thin line back out to the committed value (the part
  //               that's about to be lost)
  // When nothing is pending there's no line or tick and it looks exactly as
  // it always did.
  //
  // Flicker: every ring is described degree by degree - each whole degree is
  // one of TRACK / FILL / LINE / TICK for a given state - and a redraw
  // compares the state last drawn against the new one and repaints ONLY the
  // degrees whose content actually changed (merged into runs). Pixels that
  // look the same before and after are never touched, so dialing a change
  // only ever paints the sliver that moved, instead of wiping and redrawing
  // the whole span each encoder tick.
  // -----------------------------------------------------------------------
  enum RingKind : uint8_t
  {
    RING_TRACK = 0, // empty band: the track color (HP ring) / background (temp ring)
    RING_FILL = 1,  // full-width fill
    RING_LINE = 2,  // thin line on an otherwise empty band
    RING_TICK = 3   // full-width white tick
  };

  struct RingState
  {
    int full = 0;    // degrees [0, full) are FILL (except the tick)
    int lineTo = 0;  // degrees [full, lineTo) are LINE; == full when there's no pending change
    int tick = -1;   // the tick covers [tick-2, tick) - i.e. it ends exactly at the pending value, inside where the ring will end up; -1 for none
    uint16_t fillColor = 0; // flat style only (0 otherwise): the fill's solid color
    uint16_t lineColor = 0; // flat style only (0 otherwise): the line's solid color
    uint16_t tickColor = COL_TEXT; // the tick's color
  };

  // Where/how a ring is drawn.
  struct RingStyle
  {
    int outerR;
    int innerR;
    int lineHalf;         // half-thickness of the thin line, about the band's middle
    uint16_t trackColor;  // what an empty degree looks like
    bool smoothTrack;     // antialias the track/clear painting
    bool isTemp;          // the temp ring: solid COL_TEMP fill/line instead of the HP gradient/flat colors
  };

  static RingState s_hpRingState;
  static RingState s_tempRingState;

  static void resetRingState()
  {
    s_hpRingState = RingState();
    s_tempRingState = RingState();
  }

  // The arc [fromDeg, toDeg) of one ring band in the HP ring's style:
  // a single flat color, or the angle-keyed gradient.
  static void paintRingSpan(int outerR, int innerR, float fromDeg, float toDeg, bool flat, uint16_t flatColor)
  {
    if (toDeg <= fromDeg)
      return;
    if (flat)
      drawGaugeArc(tft, SCREEN_CX, SCREEN_CY, outerR, innerR, fromDeg, toDeg, flatColor, false);
    else
      paintGaugeGradient(tft, SCREEN_CX, SCREEN_CY, outerR, innerR, fromDeg, toDeg);
  }

  // What degree d (0..359) of a ring shows in a given state.
  static uint8_t ringKindAt(const RingState &st, int d)
  {
    // The tick is the last 2 degrees *up to* the pending value rather than
    // straddling it, so it never sticks out past where the ring finally ends:
    // when the animation finishes and the tick goes away, the end of the ring
    // is exactly where the tick's end was, with nothing beyond it to erase.
    if (st.tick >= 0 && d >= st.tick - 2 && d < st.tick)
      return RING_TICK;
    if (d < st.full)
      return RING_FILL;
    if (d < st.lineTo)
      return RING_LINE;
    return RING_TRACK;
  }

  // Paints one run [a, b) of degrees as `kind`. cleanFirst (LINE only) means
  // the band under the thin line still holds fill/tick pixels from before, so
  // it's emptied first; when it was already empty (or already a line) that
  // step is skipped so a growing line never blinks.
  static void paintRingRun(const RingStyle &rs, uint8_t kind, int a, int b, const RingState &st, bool flat, bool cleanFirst)
  {
    const int midR = (rs.outerR + rs.innerR) / 2;
    switch (kind)
    {
    case RING_TRACK:
      drawGaugeArc(tft, SCREEN_CX, SCREEN_CY, rs.outerR, rs.innerR, a, b, rs.trackColor, rs.smoothTrack);
      break;
    case RING_FILL:
      if (rs.isTemp)
        drawGaugeArc(tft, SCREEN_CX, SCREEN_CY, rs.outerR, rs.innerR, a, b, COL_TEMP);
      else
        paintRingSpan(rs.outerR, rs.innerR, a, b, flat, st.fillColor);
      break;
    case RING_LINE:
      if (cleanFirst)
        drawGaugeArc(tft, SCREEN_CX, SCREEN_CY, rs.outerR, rs.innerR, a, b, rs.trackColor, rs.smoothTrack);
      if (rs.isTemp)
        drawGaugeArc(tft, SCREEN_CX, SCREEN_CY, midR + rs.lineHalf, midR - rs.lineHalf, a, b, COL_TEMP_LINE);
      else
        paintRingSpan(midR + rs.lineHalf, midR - rs.lineHalf, a, b, flat, st.lineColor);
      break;
    case RING_TICK:
      drawGaugeArc(tft, SCREEN_CX, SCREEN_CY, rs.outerR, rs.innerR, a, b, st.tickColor, false);
      break;
    }
  }

  // Repaints exactly the degrees that differ between `cur` (what's on the
  // display) and `next`, then records `next` as what's now there.
  static void repaintRing(const RingStyle &rs, RingState &cur, const RingState &next, bool flat)
  {
    int runStart = -1;
    uint8_t runKind = RING_TRACK;
    bool runClean = false;

    for (int d = 0; d <= 360; d++) // d == 360 is a sentinel that flushes the last run
    {
      bool changed = false;
      uint8_t nk = RING_TRACK;
      bool clean = false;
      if (d < 360)
      {
        uint8_t ok = ringKindAt(cur, d);
        nk = ringKindAt(next, d);
        changed = (ok != nk) ||
                  (nk == RING_FILL && cur.fillColor != next.fillColor) ||
                  (nk == RING_LINE && cur.lineColor != next.lineColor) ||
                  (nk == RING_TICK && cur.tickColor != next.tickColor);
        clean = (nk == RING_LINE) && (ok == RING_FILL || ok == RING_TICK);
      }

      if (changed && runStart >= 0 && nk == runKind && clean == runClean)
        continue; // extends the current run

      if (runStart >= 0)
      {
        paintRingRun(rs, runKind, runStart, d, next, flat, runClean);
        runStart = -1;
      }
      if (changed)
      {
        runStart = d;
        runKind = nk;
        runClean = clean;
      }
    }
    cur = next;
  }

  // Brings the HP ring up to date. newFull/newLineTo/newTick are in degrees
  // (see the block comment above); fillColor/lineColor are only used by the
  // flat style (the gradient colors itself by angle). tickColor is used by
  // both styles.
  static void updateHpRing(float newFull, float newLineTo, float newTick, bool flat, uint16_t fillColor, uint16_t lineColor,
                           uint16_t tickColor, int outerR, int innerR)
  {
    // Thin line: the middle 6px of the band. Track repaints are antialiased
    // only in the gradient style (flat style is deliberately hard-edged -
    // see the note at its call site in showCharacterDetail()).
    const RingStyle rs = {outerR, innerR, 3, COL_TRACK, !flat, false};

    RingState next;
    next.full = (int)newFull;
    next.lineTo = (int)newLineTo;
    next.tick = (newTick >= 0) ? (int)newTick : -1;
    next.fillColor = flat ? fillColor : 0;
    next.lineColor = flat ? lineColor : 0;
    next.tickColor = tickColor;
    repaintRing(rs, s_hpRingState, next, flat);
  }

  // Same idea for the slim temp HP ring, which has no track (blank where
  // empty) and is only 4px wide, so its thin line is the middle 2px.
  static void updateTempRing(float newFull, float newLineTo, float newTick, int outerR, int innerR)
  {
    const RingStyle rs = {outerR, innerR, 1, COL_BG, true, true};

    RingState next;
    next.full = (int)newFull;
    next.lineTo = (int)newLineTo;
    next.tick = (newTick >= 0) ? (int)newTick : -1;
    next.tickColor = COL_TEMP;
    repaintRing(rs, s_tempRingState, next, true);
  }

  // Ring geometry shared by showCharacterDetail() and animateHpRings().
  // (The HP ring's inner radius is RING_INNER_R - see its own comment.)
  #define HP_RING_OUTER_R 114
  #define TEMP_RING_OUTER_R 120
  #define TEMP_RING_INNER_R 116

  // Fraction (0..1) of the ring a given amount of HP / temp HP fills.
  static float ringPct(int value, int maxHp)
  {
    if (maxHp <= 0 || value <= 0)
      return 0.0f;
    float p = (float)value / (float)maxHp;
    return p > 1.0f ? 1.0f : p;
  }

  // Flat style's solid band color for a health fraction (one of
  // GAUGE_FLAT_STEPS quantized steps along the red->yellow->green gradient).
  static uint16_t hpBandColor(float pct)
  {
    const int GAUGE_FLAT_STEPS = 40; // ~9 degrees of gradient per band
    return gaugeGradientColor(roundf(pct * GAUGE_FLAT_STEPS) / (float)GAUGE_FLAT_STEPS);
  }

  // The tick's color for a health fraction: the band color in flat style,
  // the exact gradient color in gradient style.
  static uint16_t hpTickColor(float pct, bool flat)
  {
    return flat ? hpBandColor(pct) : gaugeGradientColor(pct);
  }

  enum DetailTextRow : uint8_t
  {
    ROW_NAME1,
    ROW_NAME2,
    ROW_HP,
    ROW_TEMP,
    ROW_DELTA,
    ROW_STATUS_1,
    ROW_STATUS_2,
    ROW_COUNT
  };

  // Blanks just the area behind the text, then draws it centered at
  // (SCREEN_CX + xOffset, y) - but only when the text, color, or xOffset
  // actually changed since the last call for this row (see the equality
  // check inside). The blanked width is whichever is wider: the string
  // being drawn now, or the one drawn last time for this same row - so a
  // shorter new string (e.g. "Sent!" replacing "Sending...") still erases
  // whatever stuck out past its own edges, rather than leaving stale
  // fragments beside the new, narrower text. `row` just identifies which
  // of showCharacterDetail's fixed rows this is, purely so each one can
  // track its own previous width/text/offset - it doesn't affect where
  // anything is drawn. xOffset defaults to 0 (screen-centered, the
  // original behavior); showCharacterDetail's move-mode count sits off to
  // the side of the direction arrow instead, so it passes a nonzero one -
  // see the full-band pre-clear there for why a row whose offset can
  // change between calls needs extra care beyond what this function does
  // on its own.
  //
  // maxClearHalfWidth (default -1, meaning unclamped) caps how wide that
  // blank rectangle is ever allowed to get, regardless of how wide either
  // string measured. Rows that sit inside the HP ring's inner hole (the
  // status-tip rows near the bottom of the screen) pass ringHoleHalfWidth()
  // here, so a long tip string being replaced by a short one can't erase
  // (and, via s_lastWidth, later re-erase) pixels out past the hole and
  // into the ring's visible band. This only clamps the blanking rectangle,
  // not the text itself - a string wider than its row's safe zone still
  // draws at full width, so callers passing this should keep their
  // strings short enough to actually fit within it.
  static void drawTextRow(DetailTextRow row, int y, int rowHeight, const String &s, uint8_t size, uint16_t color,
                          bool forceRedraw = false, int xOffset = 0, int maxClearHalfWidth = -1)
  {
    static int s_lastWidth[ROW_COUNT] = {0};
    static String s_lastStr[ROW_COUNT];
    static uint16_t s_lastRowColor[ROW_COUNT] = {0};
    static int s_lastXOffset[ROW_COUNT] = {0};

    // Same text in the same color at the same offset as what's already on
    // screen for this row - skip the clear+redraw entirely. This is the
    // main source of the detail screen's flicker on a no-op background
    // refresh, since every row used to blank and repaint itself every call
    // regardless of whether its content actually changed. forceRedraw
    // bypasses this after a full fillScreen() (new character) or a
    // sub-mode switch, since the screen was just wiped out from under
    // whatever these caches remember.
    if (!forceRedraw && s == s_lastStr[row] && color == s_lastRowColor[row] && xOffset == s_lastXOffset[row])
    {
      return;
    }

    int cx = SCREEN_CX + xOffset;

    tft.setTextSize(size);
    int newWidth = (s.length() > 0) ? tft.textWidth(s) : 0;
    int clearWidth = max(newWidth, s_lastWidth[row]);
    if (maxClearHalfWidth >= 0)
    {
      clearWidth = min(clearWidth, maxClearHalfWidth * 2);
    }

    if (clearWidth > 0)
    {
      const int pad = 4; // small margin either side - cheap insurance against
                         // font-metric rounding, not a precise requirement
      tft.fillRect(cx - clearWidth / 2 - pad, y - rowHeight / 2, clearWidth + pad * 2, rowHeight, COL_BG);
    }
    s_lastWidth[row] = newWidth;
    s_lastStr[row] = s;
    s_lastRowColor[row] = color;
    s_lastXOffset[row] = xOffset;

    if (s.length() == 0)
      return;
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(color, COL_BG);
    tft.drawString(s, cx, y);
  }

  // Renders (or clears) the move-mode direction arrow at a fixed spot
  // beside the movement count text. `active` covers both "not currently in
  // move mode" and "circle mode is covering this row instead" - either
  // way, nothing should be drawn. The arrow points along the true angle of
  // (dCol, dRow) via atan2f() - screen coordinates already have +x right/
  // +y down, which is exactly the convention atan2f(dRow, dCol) assumes,
  // so no axis flipping is needed to get a visually correct heading.
  // Tracks its own tiny bit of "did anything actually change" state (angle/
  // active/has-a-vector-yet) so repeated calls with an unchanged direction
  // are cheap no-ops, same incremental-redraw idea as drawTextRow() -
  // except this is a raw shape rather than text, so it can't go through
  // that helper's text-width-based cache.
  static void drawMoveArrow(int cx, int cy, bool active, int dCol, int dRow, bool forceRedraw)
  {
    static bool s_lastActive = false;
    static bool s_lastHasVector = false;
    static float s_lastAngle = 0.0f;

    bool hasVector = active && (dCol != 0 || dRow != 0);
    float angle = hasVector ? atan2f((float)dRow, (float)dCol) : 0.0f;

    bool unchanged = !forceRedraw && active == s_lastActive && hasVector == s_lastHasVector &&
                     (!hasVector || fabsf(angle - s_lastAngle) < 0.001f);
    if (unchanged)
      return;

    const int ARROW_SIZE = 14; // tip distance from cx,cy - tune alongside CLEAR_R below if this changes
    const int CLEAR_R = ARROW_SIZE + 4;
    tft.fillRect(cx - CLEAR_R, cy - CLEAR_R, CLEAR_R * 2, CLEAR_R * 2, COL_BG);

    if (active)
    {
      if (hasVector)
      {
        float tipX = cx + ARROW_SIZE * cosf(angle);
        float tipY = cy + ARROW_SIZE * sinf(angle);
        float backLen = ARROW_SIZE * 0.9f;
        float spread = 0.5f; // radians either side of straight-back - narrower = a more needle-like arrowhead
        float backAngle = angle + PI;
        float x1 = cx + backLen * cosf(backAngle - spread);
        float y1 = cy + backLen * sinf(backAngle - spread);
        float x2 = cx + backLen * cosf(backAngle + spread);
        float y2 = cy + backLen * sinf(backAngle + spread);
        tft.fillTriangle((int)roundf(tipX), (int)roundf(tipY),
                         (int)roundf(x1), (int)roundf(y1),
                         (int)roundf(x2), (int)roundf(y2), COL_ACCENT);
      }
      else
      {
        // Just entered move mode, nothing nudged yet - a small neutral dot
        // rather than leaving the slot looking empty/broken.
        tft.fillCircle(cx, cy, 3, COL_DIM);
      }
    }
    // !active: the fillRect above already cleared it - nothing further to draw.

    s_lastActive = active;
    s_lastHasVector = hasVector;
    s_lastAngle = angle;
  }

  // Renders (or clears) circle mode's icon at the same fixed spot the move
  // arrow uses - a plain ring rather than anything directional, since
  // circle mode has no "which way" to point (the radius itself is
  // conveyed by the distance text beside it, not the icon). Same
  // cheap-no-op-on-unchanged approach as drawMoveArrow(): only active/
  // inactive can actually change here, so that's all the cache tracks.
  static void drawCircleIcon(int cx, int cy, bool active, bool forceRedraw)
  {
    static bool s_lastActive = false;

    if (!forceRedraw && active == s_lastActive)
      return;

    const int ICON_R = 12; // matches drawMoveArrow's ARROW_SIZE scale
    const int CLEAR_R = ICON_R + 4;
    tft.fillRect(cx - CLEAR_R, cy - CLEAR_R, CLEAR_R * 2, CLEAR_R * 2, COL_BG);

    if (active)
    {
      // Two concentric strokes for a bit of visual weight rather than a
      // hairline - same idea as the arrow's filled triangle reading as a
      // solid icon rather than an outline sketch.
      tft.drawCircle(cx, cy, ICON_R, COL_CIRCLE);
      tft.drawCircle(cx, cy, ICON_R - 1, COL_CIRCLE);
            tft.drawCircle(cx, cy, 4, COL_CIRCLE);
      tft.drawCircle(cx, cy, 5, COL_CIRCLE);
    }

    s_lastActive = active;
  }

  // Shared by move and circle mode's distance-beside-icon text so both
  // read the same way at a glance - see showCharacterDetail(). Rounded to
  // the nearest foot with no unit suffix - the icon (arrow vs. ring)
  // already tells you which sub-mode you're in, so the number alone reads
  // fine at this size without "ft" cluttering it up.
  static String formatDistanceFt(float distance)
  {
    return String((int)roundf(distance));
  }

  // Elevation as a plain rounded number (no unit, no "+" sign), clamped to
  // what fits in the small readout beside the name (4 characters, e.g.
  // "-999"). Shared by that readout and elevation mode's big number.
  static String formatElevation(float elevation)
  {
    int v = (int)roundf(elevation);
    if (v < -999)
      v = -999;
    if (v > 9999)
      v = 9999;
    return String(v);
  }

  // Elevation mode's icon, drawn in the same fixed spot as the move arrow/
  // circle ring: an up triangle stacked over a down triangle. Same cheap-
  // no-op-on-unchanged approach as drawCircleIcon().
  static void drawElevationIcon(int cx, int cy, bool active, bool forceRedraw)
  {
    static bool s_lastActive = false;

    if (!forceRedraw && active == s_lastActive)
      return;

    const int ICON_R = 12; // matches drawCircleIcon's ICON_R scale
    const int CLEAR_R = ICON_R + 4;
    tft.fillRect(cx - CLEAR_R, cy - CLEAR_R, CLEAR_R * 2, CLEAR_R * 2, COL_BG);

    if (active)
    {
      tft.fillTriangle(cx, cy - ICON_R, cx - 8, cy - 2, cx + 8, cy - 2, COL_ELEV); // up
      tft.fillTriangle(cx, cy + ICON_R, cx - 8, cy + 2, cx + 8, cy + 2, COL_ELEV); // down
    }

    s_lastActive = active;
  }

  // The top or bottom arc of a 2px-thick circle of radius r centered on
  // (cx, cy), trimmed to |x| <= maxX and |y| >= yMin. Drawn around the
  // number in drawSideReadout() these are the flattened caps of one big ring
  // that would enclose it - the sides, where the number is, are simply left
  // out. TFT_eSPI has no clipped drawCircle, so each stroke (radius r and
  // r-1) is drawn as a short polyline along the arc, swept from one trimmed
  // end to the other through the apex.
  static void drawRingCap(int cx, int cy, int r, int maxX, int yMin, bool top, uint16_t color)
  {
    const int sign = top ? -1 : 1;
    // Angle from straight up/down at which the arc hits the x or y trim,
    // whichever comes first.
    const float phiMax = min(asinf((float)maxX / r), acosf((float)yMin / r));
    const int SEGMENTS = 10; // per side of the apex

    for (int rr = r; rr >= r - 1; rr--)
    {
      int prevX = 0, prevY = 0;
      for (int i = -SEGMENTS; i <= SEGMENTS; i++)
      {
        float phi = phiMax * (float)i / SEGMENTS;
        int x = cx + (int)roundf(rr * sinf(phi));
        int y = cy + sign * (int)roundf(rr * cosf(phi));
        if (i > -SEGMENTS)
          tft.drawLine(prevX, prevY, x, y, color);
        prevX = x;
        prevY = y;
      }
    }
  }

  // The two small side readouts flanking the character name: elevation
  // (orange, left of the name) and the drawn circle's radius (purple, right
  // of the name). Each is the value centered in the strip of ring-hole
  // beside the name, with its icon split around it: one half above the
  // number and the other half below (an up triangle above / a down triangle
  // below for elevation, the flattened top and bottom caps of a ring that
  // would enclose the number for the circle radius) - the same idea as the
  // big mode icons, just split and scaled down.
  //
  // There isn't much room: names are at most CHAR_NAME_LINE_MAX_LEN (10)
  // characters of size-2 text, i.e. 120px centered on SCREEN_CX, which at
  // the widest leaves roughly 28px between the name and the ring's inner
  // edge on each side. So each readout lives in a fixed 28px-wide column
  // centered 78px either side of SCREEN_CX, with the number vertically
  // centered on the middle of the screen (SCREEN_CY) and the icon halves above and below it,
  // and the number is size 2 when it's 1-2 characters (the common case:
  // 5/10/15/20...) and drops to size 1 for longer ones so it never outgrows
  // the column. Drawn with its own fixed-size blanking rect rather than
  // through drawTextRow(), since that helper centers text around SCREEN_CX
  // and can't place it in a side column. An empty `text` just clears the
  // column (nothing to show: no circle drawn / no token / feature off).
  static void drawSideReadout(bool rightSide, const String &text, uint16_t color, bool ringIcon, bool forceRedraw)
  {
    static String s_lastText[2];
    const int idx = rightSide ? 1 : 0;

    if (!forceRedraw && text == s_lastText[idx])
      return;

    const int cx = rightSide ? (SCREEN_CX + 78) : (SCREEN_CX - 78);
    const int numY = SCREEN_CY;
    const int ICON_GAP = 11; // distance from the number's center line to the flat edge of each icon half (the size-2 text's half-height is 8, plus a 3px gap)
    const int ICON_H = 9;    // height of each triangle
    const int ICON_UP_SHIFT = 2; // the up triangle sits this much higher than the down one: size-2 digits only fill the top 14 of their 16px
                                 // cell, so they sit ~1px above the cell center and the gap above would otherwise look ~2px tighter than below
    const int ICON_PAD = 4;      // blanking margin beyond the icons
    const int halfW = 14; // column spans cx-14..cx+14 = 28px, clear of both the widest name and the ring's inner edge

    const int clearHalfH = ICON_GAP + ICON_H + ICON_UP_SHIFT + ICON_PAD;
    tft.fillRect(cx - halfW, SCREEN_CY - clearHalfH, halfW * 2, clearHalfH * 2, COL_BG);

    if (text.length() > 0)
    {
      const uint8_t numSize = text.length() <= 2 ? 2 : 1;
      tft.setTextSize(numSize);
      tft.setTextDatum(MC_DATUM);
      tft.setTextColor(color, COL_BG);
      // The built-in font's glyph cells are 6px wide with the 6th column
      // always blank, so a string centered on cx has its visible digits
      // sitting numSize/2 px left of cx (a full pixel at size 2) - which
      // shows up against the perfectly symmetric icons above and below it.
      // Nudge the text right by that much so the digits line up with them.
      tft.drawString(text, cx + numSize / 2, numY);

      if (ringIcon)
      {
        // Flattened top and bottom caps of one ring (radius 17) centered on
        // the number - trimmed to the 13px either side of center the column
        // has room for and to 11px above/below the center line, which clears
        // the number. Centered 1px above SCREEN_CY for the same
        // digits-sit-high reason as ICON_UP_SHIFT.
        const int ringCy = SCREEN_CY - 1;
        drawRingCap(cx, ringCy, 17, 13, 11, true, color);
        drawRingCap(cx, ringCy, 17, 13, 11, false, color);
      }
      else
      {
        // Up triangle above the number, down triangle below it.
        const int tw = 8; // half-width of each triangle's base
        const int upBase = SCREEN_CY - ICON_GAP - ICON_UP_SHIFT;
        tft.fillTriangle(cx, upBase - ICON_H, cx - tw, upBase, cx + tw, upBase, color);
        tft.fillTriangle(cx, SCREEN_CY + ICON_GAP + ICON_H, cx - tw, SCREEN_CY + ICON_GAP, cx + tw, SCREEN_CY + ICON_GAP, color);
      }
    }
    s_lastText[idx] = text;
  }

  // Smoothly extends or retracts the HP and temp HP rings from the character's
  // committed values (c.hp / c.tempHp) to the newly sent ones, e.g. right
  // after Button A sends an HP change. It continues from what the ring is
  // already showing for that pending edit (the thin line + tick between old
  // and new): healing grows the full-width fill along the line out to the
  // tick; damage shrinks the line back in toward the tick (the fill already
  // stops at the new value). The last frame is exactly the state a normal
  // redraw with the new values would produce - no line, no tick - so the
  // caller's following showCharacterDetail() has nothing left to paint.
  //
  // Blocks for the animation's duration (about 0.2-0.45s, longer for bigger
  // changes), repainting only the degrees that changed each frame (see
  // repaintRing()). Does nothing unless this character's detail screen is the
  // one currently on the display.
  void animateHpRings(const Character &c, int toHp, int toTempHp)
  {
    if (c.uuid != s_lastDetailUuid || c.maxHp <= 0)
      return;

    const bool flat = s_gaugeStyleFlat;

    float fromPct = ringPct(c.hp, c.maxHp);
    float toPct = ringPct(toHp, c.maxHp);
    float fromTempPct = ringPct(c.tempHp, c.maxHp);
    float toTempPct = ringPct(toTempHp, c.maxHp);

    const float hpFromDeg = roundf(fromPct * 360.0f);
    const float hpToDeg = roundf(toPct * 360.0f);
    const float tempFromDeg = roundf(fromTempPct * 360.0f);
    const float tempToDeg = roundf(toTempPct * 360.0f);

    float travel = max(fabsf(hpToDeg - hpFromDeg), fabsf(tempToDeg - tempFromDeg));
    if (travel <= 0.0f)
      return;

    // Longer sweeps take a little longer, within sensible bounds.
    float durF = travel * 2.5f;
    if (durF < 180.0f)
      durF = 180.0f;
    if (durF > 450.0f)
      durF = 450.0f;
    const unsigned long durationMs = (unsigned long)durF;

    const uint16_t bandColor = hpBandColor(toPct);
    const uint16_t tickColor = hpTickColor(toPct, flat);

    const unsigned long startMs = millis();
    while (true)
    {
      unsigned long elapsed = millis() - startMs;
      float t = (elapsed >= durationMs) ? 1.0f : (float)elapsed / (float)durationMs;
      float inv = 1.0f - t;
      float eased = 1.0f - inv * inv * inv; // ease-out: quick start, gentle landing

      float hpB = roundf(hpFromDeg + (hpToDeg - hpFromDeg) * eased);
      float tempB = roundf(tempFromDeg + (tempToDeg - tempFromDeg) * eased);

      float hpFull, hpLineTo, hpTick;
      if (t >= 1.0f)
      {
        hpFull = hpToDeg;
        hpLineTo = hpToDeg;
        hpTick = -1.0f;
      }
      else if (hpToDeg >= hpFromDeg)
      {
        hpFull = hpB; // healing: the fill grows toward the tick
        hpLineTo = hpToDeg;
        hpTick = (hpToDeg != hpFromDeg) ? hpToDeg : -1.0f;
      }
      else
      {
        hpFull = hpToDeg; // damage: the line shrinks toward the tick
        hpLineTo = hpB;
        hpTick = hpToDeg;
      }

      float tFull, tLineTo, tTick;
      if (t >= 1.0f)
      {
        tFull = tempToDeg;
        tLineTo = tempToDeg;
        tTick = -1.0f;
      }
      else if (tempToDeg >= tempFromDeg)
      {
        tFull = tempB;
        tLineTo = tempToDeg;
        tTick = (tempToDeg != tempFromDeg) ? tempToDeg : -1.0f;
      }
      else
      {
        tFull = tempToDeg;
        tLineTo = tempB;
        tTick = tempToDeg;
      }

      updateHpRing(hpFull, hpLineTo, hpTick, flat, bandColor, bandColor, tickColor, HP_RING_OUTER_R, RING_INNER_R);
      updateTempRing(tFull, tLineTo, tTick, TEMP_RING_OUTER_R, TEMP_RING_INNER_R);

      if (t >= 1.0f)
        break;
      delay(10); // also lets the WiFi stack run between frames
    }
  }

  void showCharacterDetail(const Character &c, HpAddTarget addTarget, int pendingHp, int pendingTempHp,
                           int pendingDelta, bool sending, bool justSent, const MovementIndicator &move, const CircleIndicator &circle,
                           const ElevationIndicator &elev)
  {
    s_listValid = false;

    // HP ring (main gauge).
    const int hpOuterR = HP_RING_OUTER_R;
    const int hpInnerR = RING_INNER_R; // kept as one shared constant (see its own comment) since drawTextRow()'s
                                       // maxClearHalfWidth calls below need to match this exactly

    // Temp HP ring: a slim accent ring just outside the HP ring. There's only
    // a few px of margin between the HP ring's outer edge and the visible
    // edge of a 240x240 round panel (center ~120,120, visible radius ~120),
    // so this stays deliberately thin, with a 1px gap so its inner edge never
    // touches the HP ring - each ring's anti-aliased edge needs genuine
    // background on both sides to blend against, not the other ring's color.
    const int tempOuterR = TEMP_RING_OUTER_R;
    const int tempInnerR = TEMP_RING_INNER_R;

    // Safe half-widths for the two status-tip rows near the bottom of the
    // screen (SCREEN_CY+65/+80) - both sit inside the HP ring's inner hole,
    // which narrows the further down you go, so their drawTextRow() calls
    // below pass these as maxClearHalfWidth to keep a long tip string being
    // replaced by a short one from blanking out past the hole and into the
    // ring's visible band (see ringHoleHalfWidth()'s comment).
    const int statusRow1HalfWidth = ringHoleHalfWidth(65);
    const int statusRow2HalfWidth = ringHoleHalfWidth(80);

    bool freshCharacter = (c.uuid != s_lastDetailUuid);
    if (freshCharacter)
    {
      tft.fillScreen(COL_BG);
      tft.drawArc(SCREEN_CX, SCREEN_CY, hpOuterR, hpInnerR, 0, 360, COL_TRACK, COL_BG, true);
      resetRingState();
      s_lastDetailUuid = c.uuid;
    }

    int displayHp = pendingHp;
    if (displayHp < 0)
      displayHp = 0;
    if (displayHp > c.maxHp)
      displayHp = c.maxHp;

    int displayTempHp = pendingTempHp;
    if (displayTempHp < 0)
      displayTempHp = 0;

    float pct = (c.maxHp > 0) ? (float)displayHp / (float)c.maxHp : 0.0f;
    if (pct > 1.0f)
      pct = 1.0f;
    if (pct < 0.0f)
      pct = 0.0f;

    uint16_t hpColor = gaugeGradientColor(pct);

    // Ring update. The rings show the *committed* values (c.hp / c.tempHp) at
    // full width, and any pending edit (pendingHp / pendingTempHp, which the
    // text below shows as numbers) as a thin line plus tick - see the block
    // comment above updateHpRing(). Degrees are snapped to whole numbers
    // here, matching the rounding toTftAngle() applies when they actually hit
    // the display, so the geometry stays pixel-consistent frame to frame
    // (pct itself stays unrounded for color purposes).
    int baseHp = c.hp;
    if (baseHp < 0)
      baseHp = 0;
    if (baseHp > c.maxHp)
      baseHp = c.maxHp;
    float basePct = (c.maxHp > 0) ? (float)baseHp / (float)c.maxHp : 0.0f;
    if (basePct > 1.0f)
      basePct = 1.0f;

    float baseDeg = roundf(basePct * 360.0f);
    float pendDeg = roundf(pct * 360.0f);
    float hpFull = min(baseDeg, pendDeg);
    float hpLineTo = max(baseDeg, pendDeg);
    float hpTick = (pendDeg != baseDeg) ? pendDeg : -1.0f;

    // Colors: the full-width fill, the thin line and the tick are all the
    // color of the *pending* health level (the same color the HP number
    // gets), so the ring previews what it will look like once the change is
    // sent. Flat style uses one solid band color for the fill and line (smooth=false
    // throughout flat mode is deliberate - antialiasing always blends toward
    // COL_BG rather than whatever a flat-mode edge actually butts up against,
    // which leaves stray flecks at the seams); the gradient style colors the
    // fill and line by angle instead, so only the tick needs the explicit
    // color there.
    uint16_t pendingBandColor = hpBandColor(pct);
    uint16_t tickColor = hpTickColor(pct, s_gaugeStyleFlat);

    updateHpRing(hpFull, hpLineTo, hpTick, s_gaugeStyleFlat, pendingBandColor, pendingBandColor, tickColor, hpOuterR, hpInnerR);

    // Temp HP ring: scaled the same way as the HP ring (fraction of maxHp,
    // capped at a full loop when temp HP >= maxHp), with the same
    // committed-value-plus-pending-line treatment.
    int baseTemp = c.tempHp;
    if (baseTemp < 0)
      baseTemp = 0;
    float baseTempPct = (c.maxHp > 0) ? (float)baseTemp / (float)c.maxHp : 0.0f;
    if (baseTempPct > 1.0f)
      baseTempPct = 1.0f;
    float tempPct = (c.maxHp > 0) ? (float)displayTempHp / (float)c.maxHp : 0.0f;
    if (tempPct > 1.0f)
      tempPct = 1.0f;

    float baseTempDeg = roundf(baseTempPct * 360.0f);
    float pendTempDeg = roundf(tempPct * 360.0f);
    updateTempRing(min(baseTempDeg, pendTempDeg), max(baseTempDeg, pendTempDeg),
                   (pendTempDeg != baseTempDeg) ? pendTempDeg : -1.0f, tempOuterR, tempInnerR);

    // Text rows - each is its own self-contained strip. drawTextRow() only
    // actually repaints a row when its text/color changed since last call
    // (or forceRedraw is set for a fresh character), so an unchanged
    // background refresh touches none of these pixels.
    String nameLine1, nameLine2;
    splitNameLines(c.name, CHAR_NAME_LINE_MAX_LEN, nameLine1, nameLine2);
    bool twoLines = nameLine2.length() > 0;

    if (twoLines)
    {
      drawTextRow(ROW_NAME1, SCREEN_CY - 20, 18, nameLine1, 2, COL_TEXT, freshCharacter);
      drawTextRow(ROW_NAME2, SCREEN_CY + 0, 18, nameLine2, 2, COL_TEXT, freshCharacter);
    }
    else
    {
      drawTextRow(ROW_NAME1, SCREEN_CY - 10, 18, nameLine1, 2, COL_TEXT, freshCharacter);
    }

    drawTextRow(ROW_HP, SCREEN_CY - 48, 26, String(displayHp) + "/" + String(c.maxHp), 3, hpColor, freshCharacter);

    // Temp HP is the only thing above the HP numbers: the "+N" at size 2,
    // centered on the screen, with a small size-1 "temp" label to its right
    // sitting on the same bottom edge as the number. The number goes through
    // drawTextRow() as usual (its blanking clamp keeps even "+125" from
    // touching the ring); the label is drawn separately since it's anchored
    // to the number's right edge rather than the screen center, so it needs
    // redrawing - and its old position blanking - whenever the number's
    // width changes.
    {
      const int tempY = SCREEN_CY - 76;
      String tempNum = displayTempHp > 0 ? ("+" + String(displayTempHp)) : String("");

      static String s_lastTempNum;
      static int s_lastTempLabelX = -1;
      bool tempChanged = freshCharacter || tempNum != s_lastTempNum;

      drawTextRow(ROW_TEMP, tempY, 18, tempNum, 2, COL_TEMP, freshCharacter, 0, ringHoleHalfWidth(85) - 4);

      if (tempChanged)
      {
        const int labelBottom = tempY + 8; // bottom edge of the size-2 text cell
        if (s_lastTempLabelX >= 0)
          tft.fillRect(s_lastTempLabelX - 1, labelBottom - 9, 28, 10, COL_BG);

        if (tempNum.length() > 0)
        {
          tft.setTextSize(2);
          int numW = tft.textWidth(tempNum);
          int labelX = SCREEN_CX + numW / 2 + 4;
          tft.setTextSize(1);
          tft.setTextDatum(BL_DATUM);
          tft.setTextColor(COL_TEMP, COL_BG);
          tft.drawString("temp", labelX, labelBottom);
          s_lastTempLabelX = labelX;
        }
        else
        {
          s_lastTempLabelX = -1;
        }
        s_lastTempNum = tempNum;
      }
    }

    // Always relative now - the dial's raw +/- delta, and which pool a
    // positive turn feeds. Subtracting always drains temp HP first regardless
    // of this setting, so it's only shown/toggled here for the add direction.
    // This row (and the status rows below, plus the direction arrow beside
    // it) are the shared sub-display: circle-editing takes it over first if
    // open (dialing the radius has to monopolize the encoder, so it gets
    // its own dedicated readout here), otherwise it shows whichever of
    // HP-edit/move the user touched most recently (see main.cpp's
    // g_lastModified) - the other one keeps running in the background
    // rather than being cancelled.
    static bool s_lastMoveActive = false;
    static bool s_lastCircleActive = false;
    static bool s_lastElevActive = false;
    bool subModeChanged = (move.active != s_lastMoveActive) || (circle.active != s_lastCircleActive) ||
                          (elev.active != s_lastElevActive);
    bool forceRowsRedraw = freshCharacter || subModeChanged;
    s_lastMoveActive = move.active;
    s_lastCircleActive = circle.active;
    s_lastElevActive = elev.active;

    if (forceRowsRedraw)
    {
      // Wipe this band wider than drawTextRow()'s own incremental clear
      // would (rather than relying on it alone): ROW_DELTA's horizontal
      // position changes between sub-modes (see the xOffset passed for
      // move mode below) and the arrow icon is a raw shape outside
      // drawTextRow()'s tracking entirely, so a same-character sub-mode
      // switch (no fillScreen()) needs an explicit clear here or stale
      // pixels from whichever sub-mode was showing before can survive the
      // switch. Bounded to BAND_HALF_WIDTH rather than the full screen
      // width, though - this row sits at SCREEN_CY+36, which is inside the
      // HP ring's inner radius (98px) directly below center, but the ring
      // itself curves back out to nearly full screen width close to the
      // edges at this same height (its donut band only exists outside
      // about +/-91px from center here). A full-width clear would slice
      // flat black notches into those two slivers of ring on the left and
      // right every time this fires; staying inside +/-91px keeps this
      // entirely within the ring's empty inner hole instead.
      const int BAND_HALF_WIDTH = 80;
      tft.fillRect(SCREEN_CX - BAND_HALF_WIDTH, (SCREEN_CY + 36) - 20, BAND_HALF_WIDTH * 2, 40, COL_BG);
    }

    // Side readouts flanking the name (see drawSideReadout()): token
    // elevation in orange on the left (showing the value being dialed while
    // elevation mode is open, so it tracks the encoder live), drawn-circle
    // radius in purple on the right. Elevation is shown only when it's non-zero (a token on the
    // ground has nothing worth showing), whether or not Height Mode is on, and
    // never for a character with no token; the radius shows whenever a circle currently exists on the
    // map for this character (circle.drawn), independent of whether the
    // encoder is dialing it (circle.active) or which of HP/move the big
    // sub-display below is showing. Drawn after the
    // band clear just above (and force-redrawn along with it) because their
    // icons sit low enough to overlap that band's top edge.
    const bool showElevation = elev.known && (int)roundf(elev.pending) != 0;
    drawSideReadout(false, showElevation ? formatElevation(elev.pending) : String(""), COL_ELEV, false, forceRowsRedraw);
    drawSideReadout(true, circle.drawn ? formatDistanceFt(circle.radius) : String(""), COL_CIRCLE, true, forceRowsRedraw);

    const int ARROW_CX = SCREEN_CX - 34;
    const int ARROW_CY = SCREEN_CY + 36;

    if (circle.active)
    {
      drawMoveArrow(ARROW_CX, ARROW_CY, false, 0, 0, forceRowsRedraw); // this row belongs to circle mode now - arrow slot stays blank
      drawElevationIcon(ARROW_CX, ARROW_CY, false, forceRowsRedraw);
      drawCircleIcon(ARROW_CX, ARROW_CY, true, forceRowsRedraw);

      String radiusStr = formatDistanceFt(circle.radius);
      drawTextRow(ROW_DELTA, SCREEN_CY + 36, 32, radiusStr, 4, COL_CIRCLE, forceRowsRedraw, 30);

      // No "Updating..." status while the radius preview is in flight - it
      // only flashed for a moment on every radius change, so the hint row
      // just stays put (circle.sending is still set by main.cpp, but unused here).
      drawTextRow(ROW_STATUS_1, SCREEN_CY + 65, 14, shortLabel(labelA()) + "/" + shortLabel(labelB()) + ":back", 1, COL_DIM, forceRowsRedraw, 0, statusRow1HalfWidth);
      drawTextRow(ROW_STATUS_2, SCREEN_CY + 80, 14, longLabel(labelA()) + "/" + longLabel(labelB()) + ":draw", 1, COL_DIM, forceRowsRedraw, 0, statusRow2HalfWidth);
      return;
    }

    if (elev.active)
    {
      drawMoveArrow(ARROW_CX, ARROW_CY, false, 0, 0, forceRowsRedraw); // this row belongs to elevation mode now - other icon slots stay blank
      drawCircleIcon(ARROW_CX, ARROW_CY, false, forceRowsRedraw);
      drawElevationIcon(ARROW_CX, ARROW_CY, true, forceRowsRedraw);

      // The value being dialed (absolute, not a +/- delta), same size/
      // position treatment as circle mode's radius.
      drawTextRow(ROW_DELTA, SCREEN_CY + 36, 32, formatElevation(elev.pending), 4, COL_ELEV, forceRowsRedraw, 30);

      if (elev.sending)
      {
        drawTextRow(ROW_STATUS_1, SCREEN_CY + 65, 14, "Sending...", 1, COL_ACCENT, forceRowsRedraw, 0, statusRow1HalfWidth);
        drawTextRow(ROW_STATUS_2, SCREEN_CY + 80, 14, " ", 1, COL_DIM, forceRowsRedraw, 0, statusRow2HalfWidth);
      }
      else
      {
        drawTextRow(ROW_STATUS_1, SCREEN_CY + 65, 14, shortLabel(labelA()) + ":set " + longLabel(labelA()) + ":cancel", 1, COL_DIM, forceRowsRedraw, 0, statusRow1HalfWidth);
        drawTextRow(ROW_STATUS_2, SCREEN_CY + 80, 14, shortLabel(labelB()) + ":back", 1, COL_DIM, forceRowsRedraw, 0, statusRow2HalfWidth);
      }
      return;
    }

    if (move.active)
    {
      drawCircleIcon(ARROW_CX, ARROW_CY, false, forceRowsRedraw); // this row belongs to move mode now - circle icon slot stays blank
      drawElevationIcon(ARROW_CX, ARROW_CY, false, forceRowsRedraw);
      drawMoveArrow(ARROW_CX, ARROW_CY, true, move.dCol, move.dRow, forceRowsRedraw);

      // Distance to the accumulated target, beside the arrow - same size/
      // position/units treatment as circle mode's radius, so the two
      // sub-modes read the same way at a glance. Only known once the
      // first preview response comes back (see main.cpp's sendMovePreview()),
      // so this reads "0" for the single frame between entering move mode
      // and that response landing.
      String distStr = formatDistanceFt(move.distance);
      drawTextRow(ROW_DELTA, SCREEN_CY + 36, 32, distStr, 4, COL_ACCENT, forceRowsRedraw, 30);

      // No "Moving..." status while a preview is in flight - same reasoning
      // as the circle branch above; the hint row just stays put.
      drawTextRow(ROW_STATUS_1, SCREEN_CY + 65, 14, shortLabel(labelA()) + ":move " + longLabel(labelA()) + ":back", 1, COL_DIM, forceRowsRedraw, 0, statusRow1HalfWidth);
      // Second hint row intentionally left blank in move mode: the Button B
      // ":elev" / ":ring" hints that used to live here were removed to keep
      // this screen uncluttered.
      drawTextRow(ROW_STATUS_2, SCREEN_CY + 80, 14, String(""), 1, COL_DIM, forceRowsRedraw, 0, statusRow2HalfWidth);
      return;
    }

    // Neither move nor circle-editing is the current sub-display: normal
    // HP editing. Make sure both icon slots are blank (e.g. just switched
    // back from showing the move) before falling through to the usual
    // pending-HP-delta rendering.
    drawMoveArrow(ARROW_CX, ARROW_CY, false, 0, 0, forceRowsRedraw);
    drawCircleIcon(ARROW_CX, ARROW_CY, false, forceRowsRedraw);
    drawElevationIcon(ARROW_CX, ARROW_CY, false, forceRowsRedraw);

    String deltaStr = (pendingDelta > 0 ? "+" : "") + String(pendingDelta);
    deltaStr = (pendingDelta == 0 ? "-" : deltaStr);

    if (pendingDelta >= 0)
    {
      if (addTarget == HpAddTarget::TEMP)
      {
        drawTextRow(ROW_DELTA, SCREEN_CY + 36, 32, deltaStr, 4, COL_TEMP, forceRowsRedraw);
      }
      else
      {
        if (pendingDelta == 0)
        {
          drawTextRow(ROW_DELTA, SCREEN_CY + 36, 32, deltaStr, 4, COL_DIM, forceRowsRedraw);
        }
        else
        {
          drawTextRow(ROW_DELTA, SCREEN_CY + 36, 32, deltaStr, 4, COL_HP_GOOD, forceRowsRedraw);
        }
      }
    }
    else if (pendingDelta < 0)
    {
      drawTextRow(ROW_DELTA, SCREEN_CY + 36, 32, deltaStr, 4, COL_HP_BAD, forceRowsRedraw);
    }
    else
    {
      drawTextRow(ROW_DELTA, SCREEN_CY + 36, 32, " ", 4, COL_DIM, forceRowsRedraw);
    }

    if (sending || elev.sending) // elevation is applied on screen right away, so its "Sending..." shows here, outside elevation mode
    {
      drawTextRow(ROW_STATUS_1, SCREEN_CY + 65, 14, "Sending...", 1, COL_ACCENT, forceRowsRedraw, 0, statusRow1HalfWidth);
      drawTextRow(ROW_STATUS_2, SCREEN_CY + 80, 14, " ", 1, COL_DIM, forceRowsRedraw);
    }
    else if (justSent)
    {
      drawTextRow(ROW_STATUS_1, SCREEN_CY + 65, 14, "Sent!", 1, COL_HP_GOOD, forceRowsRedraw);
      drawTextRow(ROW_STATUS_2, SCREEN_CY + 80, 14, " ", 1, COL_DIM, forceRowsRedraw);
    }
    else
    {
      drawTextRow(ROW_STATUS_1, SCREEN_CY + 65, 14, shortLabel(labelA()) + ":send HP " + longLabel(labelA()) + ":back", 1, COL_DIM, forceRowsRedraw, 0, statusRow1HalfWidth);
      if (addTarget == HpAddTarget::TEMP)
      {
        drawTextRow(ROW_STATUS_2, SCREEN_CY + 80, 14, doubleLabel(labelA()) + ":reg hp", 1, COL_DIM, forceRowsRedraw, 0, statusRow2HalfWidth);
      }
      else
      {
        drawTextRow(ROW_STATUS_2, SCREEN_CY + 80, 14, doubleLabel(labelA()) + ":temp hp", 1, COL_DIM, forceRowsRedraw, 0, statusRow2HalfWidth);
      }
    }
  }

} // namespace Display