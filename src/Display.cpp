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
#define COL_CIRCLE 0xFBE0 // warm orange for the persistent circle-radius readout - distinct from temp hp's blue and the cyan accent

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
    // Read a row at a time into a small stack buffer via memcpy_P rather
    // than pushImage()'ing straight from the PROGMEM array: ESP8266 flash
    // reads need to be word-aligned, and dereferencing a PROGMEM pointer
    // directly (which pushImage would do internally) risks an unaligned
    // read exception. memcpy_P handles that correctly, and one row
    // (190 * 2 = 380 bytes here) is a trivial amount of stack.
    int x = (tft.width() - CHAR_O_MATIC_LOGO_W) / 2;
    int y = (tft.height() - CHAR_O_MATIC_LOGO_H) / 2 - 40;
    uint16_t rowBuf[CHAR_O_MATIC_LOGO_W];
    for (int row = 0; row < CHAR_O_MATIC_LOGO_H; row++)
    {
      memcpy_P(rowBuf, &CHAR_O_MATIC_LOGO_RGB565[row * CHAR_O_MATIC_LOGO_W], sizeof(rowBuf));
      tft.pushImage(x, y + row, CHAR_O_MATIC_LOGO_W, 1, rowBuf);
    }
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

  // Returns an interpolated red->yellow->green color for a gauge position,
  // where frac 0.0 is the empty end (red) and 1.0 is the full end (green).
  static uint16_t gaugeGradientColor(float frac)
  {
    if (frac < 0.0f)
      frac = 0.0f;
    if (frac > 1.0f)
      frac = 1.0f;
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

  enum DetailTextRow : uint8_t
  {
    ROW_NAME1,
    ROW_NAME2,
    ROW_HP,
    ROW_TEMP,
    ROW_CIRCLE,
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

  void showCharacterDetail(const Character &c, HpAddTarget addTarget, int pendingHp, int pendingTempHp,
                           int pendingDelta, bool sending, bool justSent, const MovementIndicator &move, const CircleIndicator &circle)
  {
    s_listValid = false;

    static float s_lastPct = -1.0f;
    static uint16_t s_lastColor = 0;
    static float s_lastTempPct = -1.0f;

    // HP ring (main gauge).
    const int hpOuterR = 114;
    const int hpInnerR = RING_INNER_R; // kept as one shared constant (see its own comment) since drawTextRow()'s
                                       // maxClearHalfWidth calls below need to match this exactly

    // Temp HP ring: a slim accent ring just outside the HP ring. There's only
    // a few px of margin between the HP ring's outer edge and the visible
    // edge of a 240x240 round panel (center ~120,120, visible radius ~120),
    // so this stays deliberately thin, with a 1px gap so its inner edge never
    // touches the HP ring - each ring's anti-aliased edge needs genuine
    // background on both sides to blend against, not the other ring's color.
    const int tempOuterR = 120;
    const int tempInnerR = 116;

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
      s_lastPct = 0.0f;
      s_lastTempPct = 0.0f;
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

    // Ring update: only repaint the part of the arc that actually changed.
    // If the color band changed (e.g. green -> yellow) we have to repaint
    // the whole filled portion, since the old color is baked into pixels we
    // can't selectively recolor otherwise.
    // Snap to whole degrees here, matching the rounding toTftAngle() applies
    // when these values actually hit the display. Only the ring geometry
    // (this boundary) needs to be pixel-consistent frame to frame; pct
    // itself stays unrounded for color purposes below. Without this, oldDeg
    // here and the boundary actually painted last frame can round to
    // different integer degrees, leaving a real, fully-opaque leftover
    // pixel of stale color between where this frame's erase starts and
    // where last frame's fill actually stopped.
    float newDeg = roundf(pct * 360.0f);
    float oldDeg = roundf(s_lastPct * 360.0f);
    ///
    if (s_gaugeStyleFlat)
    {
      const int GAUGE_FLAT_STEPS = 40; // ~9 degrees of gradient per repaint step
      uint16_t hpColorQuantized = gaugeGradientColor(round(pct * GAUGE_FLAT_STEPS) / (float)GAUGE_FLAT_STEPS);

      // Grow/shrink the boundary exactly as before - independent of whether
      // the color changed this tick. smooth=false on all three flat-mode
      // calls (this pair plus the recolor below) is deliberate: smoothArc's
      // antialiasing always blends edge pixels toward the fixed COL_BG
      // passed to TFT_eSPI, not toward whatever color the call is actually
      // butting up against (hpColor here, COL_TRACK there) - fine when that
      // neighbor really is COL_BG, wrong otherwise, and it only takes one
      // side of a seam being wrong to leave a stray fleck. Keeping every
      // flat-mode edge hard avoids the mismatch entirely instead of trying
      // to get AA to agree with itself across separate calls; flat mode's
      // whole aesthetic is solid bands anyway, so losing the soft edge here
      // costs nothing. Gradient mode below is untouched since it isn't
      // reported to have this problem and the smoothing matters more there.
      if (newDeg > oldDeg)
      {
        drawGaugeArc(tft, SCREEN_CX, SCREEN_CY, hpOuterR, hpInnerR, oldDeg, newDeg, hpColor, false);
      }
      else if (newDeg < oldDeg)
      {
        drawGaugeArc(tft, SCREEN_CX, SCREEN_CY, hpOuterR, hpInnerR, newDeg, oldDeg, COL_TRACK, false);
      }

      // If the color band shifted, only the already-filled region (0->newDeg)
      // is now the wrong color - the track portion (newDeg->360) was never
      // hpColor to begin with, so it doesn't need touching. Repainting just
      // the fill region instead of the whole ring avoids a full-circumference
      // wipe+redraw (and the flicker that came with it) on every color step.
      if (hpColorQuantized != s_lastColor && newDeg > 0)
      {
        drawGaugeArc(tft, SCREEN_CX, SCREEN_CY, hpOuterR, hpInnerR, 0, newDeg, hpColor, false);
      }
      s_lastColor = hpColorQuantized;
    }
    else
    {
      if (newDeg > oldDeg)
      {
        paintGaugeGradient(tft, SCREEN_CX, SCREEN_CY, hpOuterR, hpInnerR, oldDeg, newDeg);
      }
      else if (newDeg < oldDeg)
      {
        drawGaugeArc(tft, SCREEN_CX, SCREEN_CY, hpOuterR, hpInnerR, newDeg, oldDeg, COL_TRACK);
      }
    }
    s_lastPct = newDeg / 360.0f;
    ///
    // Temp HP ring: scaled the same way as the HP ring (fraction of maxHp,
    // capped at a full loop when temp HP >= maxHp). Unlike the HP ring there's
    // no background "track" color - it's just blank (COL_BG) when there's no
    // temp HP, and grows/shrinks in blue as temp HP is gained/spent.
    float tempPct = (c.maxHp > 0) ? (float)displayTempHp / (float)c.maxHp : 0.0f;
    if (tempPct > 1.0f)
      tempPct = 1.0f;

    float newTempDeg = roundf(tempPct * 360.0f);
    float oldTempDeg = roundf(s_lastTempPct * 360.0f);

    if (newTempDeg > oldTempDeg)
    {
      drawGaugeArc(tft, SCREEN_CX, SCREEN_CY, tempOuterR, tempInnerR, oldTempDeg, newTempDeg, COL_TEMP);
    }
    else if (newTempDeg < oldTempDeg)
    {
      drawGaugeArc(tft, SCREEN_CX, SCREEN_CY, tempOuterR, tempInnerR, newTempDeg, oldTempDeg, COL_BG);
    }
    s_lastTempPct = newTempDeg / 360.0f;

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

    drawTextRow(ROW_TEMP, SCREEN_CY - 82, 14, displayTempHp > 0 ? ("+" + String(displayTempHp) + " temp") : "", 1, COL_TEMP, freshCharacter);

    // Circle radius readout, styled the same way as the temp HP row right
    // above it - shown whenever a circle currently exists on the map for
    // this character (circle.drawn), independent of whether the encoder is
    // actively dialing it right now (circle.active, handled separately
    // below) or of whichever of HP/move the big sub-display area is
    // currently showing. Blank (and so invisible) when no circle is drawn,
    // same on/off pattern as the temp-HP row's "only show when > 0" check.
    drawTextRow(ROW_CIRCLE, SCREEN_CY - 70, 14, circle.drawn ? (formatDistanceFt(circle.radius) + " ft radius") : "", 1, COL_CIRCLE, freshCharacter);

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
    bool subModeChanged = (move.active != s_lastMoveActive) || (circle.active != s_lastCircleActive);
    bool forceRowsRedraw = freshCharacter || subModeChanged;
    s_lastMoveActive = move.active;
    s_lastCircleActive = circle.active;

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

    const int ARROW_CX = SCREEN_CX - 34;
    const int ARROW_CY = SCREEN_CY + 36;

    if (circle.active)
    {
      drawMoveArrow(ARROW_CX, ARROW_CY, false, 0, 0, forceRowsRedraw); // this row belongs to circle mode now - arrow slot stays blank
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

    if (move.active)
    {
      drawCircleIcon(ARROW_CX, ARROW_CY, false, forceRowsRedraw); // this row belongs to move mode now - circle icon slot stays blank
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
      drawTextRow(ROW_STATUS_2, SCREEN_CY + 80, 14, circle.enabled ? (shortLabel(labelB()) + ":ring") : String(""), 1, COL_DIM, forceRowsRedraw, 0, statusRow2HalfWidth);
      return;
    }

    // Neither move nor circle-editing is the current sub-display: normal
    // HP editing. Make sure both icon slots are blank (e.g. just switched
    // back from showing the move) before falling through to the usual
    // pending-HP-delta rendering.
    drawMoveArrow(ARROW_CX, ARROW_CY, false, 0, 0, forceRowsRedraw);
    drawCircleIcon(ARROW_CX, ARROW_CY, false, forceRowsRedraw);

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

    if (sending)
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