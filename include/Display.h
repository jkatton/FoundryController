#pragma once
#include <Arduino.h>
#include <vector>
#include "Types.h"

namespace Display {

  void begin();

  // Applies a TFT_eSPI rotation (0-3) immediately, if it's actually
  // different from the current one, and invalidates cached screen state so
  // whatever gets shown next is a full repaint at the new geometry. Safe to
  // call before any other show*() - begin() itself uses the same default
  // this starts at.
  void setRotation(uint8_t rotation);

  // Switches the HP ring between the flat quantized-band style and the
  // smooth per-degree gradient (previously the compile-time GAUGE_STYLE_FLAT
  // #if in config.h - now a runtime setting so it can live on the settings
  // screen). Takes effect the next time showCharacterDetail() repaints the
  // ring, e.g. on the next character switch.
  void setGaugeStyleFlat(bool flat);

  void showBoot(const String &line);

  // Draws the Char-o-matic wordmark centered on the display, on a plain
  // COL_BG background. Standalone splash screen - call it, let it sit on
  // screen for whatever duration the caller wants, then move on to the
  // next showXxx() (there's no auto-timeout or fade built in here).
  void showLogo();
  void showWifiPortalHint();              // shown right before WiFiManager's blocking autoConnect()
  void showRelayConnecting(const String &host, uint16_t port);
  void showError(const String &message);

  // IP entry screen: 4 octets + port, one field editable at a time.
  // fieldIndex: 0-3 = IP octets, 4 = port
  void showIpEntry(const uint16_t fields[5], int fieldIndex, bool fakery);

  void showLoadingCharacters();

  // Settings carousel: one row per user-editable setting, names[i]/values[i]
  // paired by index, in carousel order. selectedIndex is the row drawn
  // highlighted in the middle - same windowed-carousel look as
  // showCharacterList(). Rendering has no knowledge of what each setting
  // actually does; main.cpp's SETTINGS_TABLE owns that, so adding a new
  // setting never requires touching this function.
  void showSettings(const std::vector<String> &names, const std::vector<String> &values, int selectedIndex);

  // scrollOffset/selectedIndex let the caller manage a simple windowed list.
  void showCharacterList(const std::vector<Character> &chars, int selectedIndex);

void showCharacterDetail(const Character &c, HpAddTarget addTarget, int pendingHp, int pendingTempHp,
                          int pendingDelta, bool sending, bool justSent,
                          const MovementIndicator &move = MovementIndicator(),
                          const CircleIndicator &circle = CircleIndicator());

}