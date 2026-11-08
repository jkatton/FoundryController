#pragma once
#include <Arduino.h>

// Unit-to-unit firmware transfer over ESP-NOW - no router, no WiFi
// credentials, no IP addresses. Used by the ESP-NOW mode that main.cpp
// enters when the unit is powered on with both buttons held.
//
// Both entry points BLOCK until the transfer finishes, fails, or the user
// cancels (the `cancelled` callback returns true). While blocked they keep
// the radio serviced and call back into main.cpp to paint the screen, so
// this module has no display or input dependencies of its own.
//
// Protocol (stop-and-wait, ESP-NOW channel ESPNOW_CHANNEL):
//   sender   -> broadcast  DISCOVER   (repeated until someone answers)
//   receiver -> sender     HELLO      (version + free flash space)
//   sender   -> receiver   START      (image size + MD5)
//   receiver -> sender     START_ACK  (receiver has called Update.begin())
//   sender   -> receiver   DATA seq   (ESPNOW_CHUNK_BYTES each)
//   receiver -> sender     ACK  seq   (sender retransmits on timeout)
//   sender   -> receiver   END
//   receiver -> sender     RESULT     (image verified against the MD5)
// START carries the sender's version. If it isn't newer than the receiver's
// own, the receiver answers START_ACK "needs confirm", asks its user, and the
// sender keeps re-sending START until the answer comes (a refusal arrives as
// ABORT).
// Either side can send ABORT. The running firmware on the receiver is never
// touched until the whole image has arrived AND matched the MD5.
namespace EspNowOta {

  struct Callbacks {
    // Full-screen status. footer is the dim line at the bottom.
    void (*status)(const String &line1, const String &line2, const String &footer);
    // Transfer progress, 0-100. Only called when the number changes.
    void (*progress)(int percent);
    // Return true when the user asked to cancel. Called often.
    bool (*cancelled)();
    // Version check (receiver only). Called when the incoming firmware is not
    // newer than the running one: prompt() paints the question (and must
    // clear any remembered answer); confirm() is then polled and returns
    // 1 = install, -1 = refuse, 0 = no answer yet. Newer firmware installs
    // without asking.
    void (*prompt)(const String &line1, const String &line2);
    int (*confirm)();
  };

  enum class Result { SUCCESS, CANCELLED, FAILED };

  // Waits for a sender, receives an image, verifies and commits it. On
  // SUCCESS the new firmware is staged and the caller should reboot.
  // On FAILED, `err` holds a short message (<= ~28 chars).
  Result receiveFirmware(const Callbacks &cb, String &err);

  // Finds a receiver, then streams this unit's own running firmware to it.
  // On SUCCESS the receiver has verified the image and is rebooting.
  Result sendFirmware(const Callbacks &cb, String &err);

}