#include "EspNowOta.h"
#include "config.h"

#include <ESP8266WiFi.h>
#include <Updater.h>
#include <MD5Builder.h>

extern "C" {
#include <espnow.h>
#include <user_interface.h>
}

namespace EspNowOta
{

// ---------------------------------------------------------------------
// Wire format
// ---------------------------------------------------------------------
static const uint8_t PKT_MAGIC = 0xC0;

enum : uint8_t
{
  PKT_DISCOVER = 1,
  PKT_HELLO = 2,
  PKT_START = 3,
  PKT_START_ACK = 4,
  PKT_DATA = 5,
  PKT_ACK = 6,
  PKT_END = 7,
  PKT_RESULT = 8,
  PKT_ABORT = 9
};

enum : uint8_t
{
  ST_OK = 0,
  ST_NO_SPACE = 1,
  ST_WRITE_FAIL = 2,
  ST_VERIFY_FAIL = 3,
  ST_BAD_REQUEST = 4,
  ST_NEEDS_CONFIRM = 5 // receiver is asking its user; keep re-sending START
};

struct __attribute__((packed)) Header
{
  uint8_t magic;
  uint8_t type;
  uint8_t session; // random per send attempt; strays from other sessions are ignored
  uint16_t seq;    // DATA/ACK sequence number
};
static const size_t HDR = sizeof(Header);

struct __attribute__((packed)) VersionBody // DISCOVER
{
  char version[12];
};
struct __attribute__((packed)) HelloBody // HELLO
{
  char version[12];
  uint32_t freeSpace;
};
struct __attribute__((packed)) StartBody // START
{
  uint32_t size;
  char md5[32];
  char version[12];
};

static_assert(HDR + ESPNOW_CHUNK_BYTES <= 250, "ESP-NOW payload limit is 250 bytes");
static_assert((ESPNOW_CHUNK_BYTES % 4) == 0, "chunk size must be a multiple of 4 (flash reads are word aligned)");

// "1.2.1" style, compared numerically field by field. Returns 1 if a > b,
// -1 if a < b, 0 if equal. Missing/garbled fields count as 0.
static int parseVersionField(const char *&p)
{
  int v = 0;
  while (*p && *p != '.')
  {
    if (*p >= '0' && *p <= '9')
      v = v * 10 + (*p - '0');
    p++;
  }
  if (*p == '.')
    p++;
  return v;
}

static int compareVersions(const char *a, const char *b)
{
  for (int i = 0; i < 4; i++)
  {
    int x = parseVersionField(a), y = parseVersionField(b);
    if (x != y)
      return x > y ? 1 : -1;
  }
  return 0;
}

static const uint8_t BROADCAST_MAC[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// ---------------------------------------------------------------------
// Radio plumbing. The receive callback only copies packets into a small
// ring; everything else (flash writes, replies) happens from the blocking
// loops below, never from inside the WiFi task.
// ---------------------------------------------------------------------
struct RxPacket
{
  uint8_t mac[6];
  uint8_t len;
  uint8_t data[250];
};

static const uint8_t RX_SLOTS = 6;
static RxPacket s_rx[RX_SLOTS];
static volatile uint8_t s_rxHead = 0;
static volatile uint8_t s_rxTail = 0;
static volatile bool s_sendBusy = false;
static bool s_radioUp = false;

static void onRecv(uint8_t *mac, uint8_t *data, uint8_t len)
{
  if (len < HDR || data[0] != PKT_MAGIC)
    return;
  uint8_t next = (s_rxHead + 1) % RX_SLOTS;
  if (next == s_rxTail)
    return; // ring full: drop it, the sender retransmits
  RxPacket &p = s_rx[s_rxHead];
  memcpy(p.mac, mac, 6);
  p.len = len;
  memcpy(p.data, data, len);
  s_rxHead = next;
}

static void onSent(uint8_t *mac, uint8_t status)
{
  s_sendBusy = false;
}

static bool popPacket(RxPacket &out)
{
  if (s_rxTail == s_rxHead)
    return false;
  out = s_rx[s_rxTail];
  s_rxTail = (s_rxTail + 1) % RX_SLOTS;
  return true;
}

static bool parseHeader(const RxPacket &p, Header &h)
{
  if (p.len < HDR)
    return false;
  memcpy(&h, p.data, HDR);
  return h.magic == PKT_MAGIC;
}

static void ensurePeer(const uint8_t *mac)
{
  if (!esp_now_is_peer_exist((uint8_t *)mac))
    esp_now_add_peer((uint8_t *)mac, ESP_NOW_ROLE_COMBO, ESPNOW_CHANNEL, NULL, 0);
}

static bool radioBegin(String &err)
{
  // This mode is only entered at boot, before any WiFi connection. Make
  // sure the SDK isn't trying to (re)join a saved network - that would hop
  // channels out from under ESP-NOW.
  WiFi.persistent(false);
  WiFi.setAutoConnect(false);
  WiFi.setAutoReconnect(false);
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  WiFi.setSleepMode(WIFI_NONE_SLEEP); // modem sleep makes a receiver miss packets
  wifi_set_channel(ESPNOW_CHANNEL);

  if (s_radioUp)
  {
    esp_now_deinit();
    s_radioUp = false;
  }
  if (esp_now_init() != 0)
  {
    err = "ESP-NOW init failed";
    return false;
  }
  s_radioUp = true;
  esp_now_set_self_role(ESP_NOW_ROLE_COMBO);
  esp_now_register_recv_cb(onRecv);
  esp_now_register_send_cb(onSent);

  s_rxHead = s_rxTail = 0;
  s_sendBusy = false;
  ensurePeer(BROADCAST_MAC);
  return true;
}

static void radioEnd()
{
  if (s_radioUp)
  {
    esp_now_unregister_recv_cb();
    esp_now_unregister_send_cb();
    esp_now_deinit();
    s_radioUp = false;
  }
}

// Builds header+body in one static buffer and fires it. Waits for the
// previous send to finish first (so the buffer is safe to reuse).
static void sendPacket(const uint8_t *mac, uint8_t type, uint8_t session, uint16_t seq,
                       const void *body = nullptr, size_t bodyLen = 0)
{
  static uint8_t buf[250];
  unsigned long t0 = millis();
  while (s_sendBusy && millis() - t0 < 30)
    delay(1);

  Header h = {PKT_MAGIC, type, session, seq};
  memcpy(buf, &h, HDR);
  if (bodyLen)
    memcpy(buf + HDR, body, bodyLen);
  s_sendBusy = true;
  if (esp_now_send((uint8_t *)mac, buf, HDR + bodyLen) != 0)
    s_sendBusy = false;
}

static String macToString(const uint8_t *mac)
{
  char b[18];
  snprintf(b, sizeof(b), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return String(b);
}

// Throttled so a tight wait loop doesn't hammer the I2C expander.
static bool userCancelled(const Callbacks &cb)
{
  static unsigned long last = 0;
  unsigned long now = millis();
  if (now - last < 20)
    return false;
  last = now;
  return cb.cancelled && cb.cancelled();
}

static void reportProgress(const Callbacks &cb, uint32_t done, uint32_t total, int &lastPct)
{
  int pct = (int)((done * 100ULL) / (total ? total : 1));
  if (pct != lastPct)
  {
    lastPct = pct;
    if (cb.progress)
      cb.progress(pct);
  }
}

// =====================================================================
// RECEIVER
// =====================================================================
Result receiveFirmware(const Callbacks &cb, String &err)
{
  if (!radioBegin(err))
    return Result::FAILED;

  Result result = Result::FAILED;
  err = "";

  bool active = false;        // a START has been accepted
  bool finalized = false;     // whole image received and Update.end() has run
  bool resultSent = false;
  uint8_t finalStatus = ST_OK;
  uint8_t peerMac[6] = {0};
  uint8_t session = 0;
  uint32_t total = 0, received = 0;
  uint16_t expectSeq = 0;
  // Version-check prompt in progress (incoming firmware not newer than ours)
  bool pending = false, approved = false;
  uint8_t pendingMac[6] = {0};
  uint8_t pendingSession = 0;
  unsigned long pendingLastMs = 0;
  unsigned long lastRxMs = 0, resultSentMs = 0, finalizedMs = 0;
  int lastPct = -1;

  cb.status("Waiting for sender...", "Pick Send on the other unit", "Hold any button to cancel");

  for (;;)
  {
    unsigned long now = millis();

    if (!finalized && userCancelled(cb))
    {
      if (active)
      {
        sendPacket(peerMac, PKT_ABORT, session, 0);
        Update.end(); // image incomplete -> this aborts the update
      }
      else if (pending)
        sendPacket(pendingMac, PKT_ABORT, pendingSession, 0);
      result = Result::CANCELLED;
      break;
    }

    RxPacket p;
    bool fatal = false;
    while (!fatal && popPacket(p))
    {
      Header h;
      if (!parseHeader(p, h))
        continue;
      const uint8_t *body = p.data + HDR;
      const size_t bodyLen = p.len - HDR;
      const bool fromPeer = active && memcmp(p.mac, peerMac, 6) == 0 && h.session == session;

      switch (h.type)
      {
      case PKT_DISCOVER:
      {
        if (active)
          break;
        ensurePeer(p.mac);
        HelloBody hello;
        memset(&hello, 0, sizeof(hello));
        strncpy(hello.version, FIRMWARE_VERSION, sizeof(hello.version) - 1);
        hello.freeSpace = ESP.getFreeSketchSpace();
        sendPacket(p.mac, PKT_HELLO, h.session, 0, &hello, sizeof(hello));
        break;
      }

      case PKT_START:
      {
        if (bodyLen < sizeof(StartBody))
          break;
        if (fromPeer)
        { // sender didn't see our START_ACK; just repeat it
          uint8_t ok = ST_OK;
          sendPacket(peerMac, PKT_START_ACK, session, 0, &ok, 1);
          break;
        }
        if (active)
          break; // someone else's transfer is in progress

        StartBody sb;
        memcpy(&sb, body, sizeof(sb));
        char md5[33];
        memcpy(md5, sb.md5, 32);
        md5[32] = 0;
        char senderVer[13];
        memcpy(senderVer, sb.version, 12);
        senderVer[12] = 0;

        const bool samePending = pending && memcmp(p.mac, pendingMac, 6) == 0 && h.session == pendingSession;
        if (pending && !samePending)
          break; // already asking the user about a different sender

        // Version check: strictly newer installs automatically; same or
        // older needs the user's OK on this unit.
        const int cmp = compareVersions(senderVer, FIRMWARE_VERSION);
        if (cmp <= 0 && !(samePending && approved))
        {
          if (!samePending)
          {
            pending = true;
            approved = false;
            memcpy(pendingMac, p.mac, 6);
            pendingSession = h.session;
            ensurePeer(p.mac);
            String l1 = String(cmp == 0 ? "Same version v" : "Older version v") + senderVer;
            String l2 = String("This unit: v") + FIRMWARE_VERSION;
            if (cb.prompt)
              cb.prompt(l1, l2);
          }
          pendingLastMs = millis();
          uint8_t wait = ST_NEEDS_CONFIRM;
          sendPacket(p.mac, PKT_START_ACK, h.session, 0, &wait, 1);
          break;
        }
        pending = false;
        approved = false;

        uint8_t status = ST_OK;
        ensurePeer(p.mac);
        if (sb.size == 0 || sb.size > ESP.getFreeSketchSpace())
          status = ST_NO_SPACE;
        else
        {
          Update.clearError();
          if (!Update.begin(sb.size))
            status = ST_NO_SPACE;
          else
            Update.setMD5(md5); // after begin(): begin() resets the updater
        }

        if (status == ST_OK)
        {
          active = true;
          memcpy(peerMac, p.mac, 6);
          session = h.session;
          total = sb.size;
          received = 0;
          expectSeq = 0;
          lastRxMs = millis();
          cb.status("Receiving...", "from " + macToString(peerMac), "Do not power off");
          reportProgress(cb, 0, total, lastPct);
        }
        sendPacket(p.mac, PKT_START_ACK, h.session, 0, &status, 1);
        if (status != ST_OK)
        {
          err = "Image won't fit here";
          // stay in the waiting state: another sender may try again
        }
        break;
      }

      case PKT_DATA:
      {
        if (!fromPeer || finalized)
        {
          if (fromPeer && h.seq < expectSeq)
            sendPacket(peerMac, PKT_ACK, session, h.seq); // late duplicate after finalize
          break;
        }
        lastRxMs = millis();

        if (h.seq < expectSeq)
        { // our ACK got lost; re-ACK without writing it twice
          sendPacket(peerMac, PKT_ACK, session, h.seq);
          break;
        }
        if (h.seq > expectSeq)
          break; // gap: ignore, sender will retransmit the missing one

        if (received + bodyLen > total)
        {
          uint8_t st = ST_BAD_REQUEST;
          sendPacket(peerMac, PKT_RESULT, session, 0, &st, 1);
          Update.end();
          err = "Bad data from sender";
          fatal = true;
          break;
        }
        if (Update.write((uint8_t *)body, bodyLen) != bodyLen)
        {
          sendPacket(peerMac, PKT_ABORT, session, 0);
          Update.end();
          err = "Flash write failed";
          fatal = true;
          break;
        }
        received += bodyLen;
        expectSeq++;
        sendPacket(peerMac, PKT_ACK, session, h.seq);
        reportProgress(cb, received, total, lastPct);

        if (received == total)
        {
          // Last byte is in: verify the MD5 and stage the image now.
          finalStatus = Update.end() ? ST_OK : ST_VERIFY_FAIL;
          finalized = true;
          finalizedMs = millis();
        }
        break;
      }

      case PKT_END:
      {
        if (!fromPeer)
          break;
        uint8_t st = finalized ? finalStatus : ST_BAD_REQUEST;
        sendPacket(peerMac, PKT_RESULT, session, 0, &st, 1);
        if (!finalized)
        {
          Update.end();
          err = "Incomplete transfer";
          fatal = true;
          break;
        }
        if (!resultSent)
        {
          resultSent = true;
          resultSentMs = millis();
        }
        break;
      }

      case PKT_ABORT:
      {
        if (!fromPeer || finalized)
          break;
        Update.end();
        err = "Sender cancelled";
        fatal = true;
        break;
      }

      default:
        break;
      }
    }

    if (fatal)
    {
      result = Result::FAILED;
      break;
    }

    // Version-check prompt: act on the user's answer. "Install" is picked up
    // by the sender's next START (it repeats about every 0.5s).
    if (pending && !active)
    {
      int answer = cb.confirm ? cb.confirm() : -1;
      if (answer < 0)
      {
        sendPacket(pendingMac, PKT_ABORT, pendingSession, 0); // refusal
        pending = false;
        approved = false;
        cb.status("Waiting for sender...", "Pick Send on the other unit", "Hold any button to cancel");
      }
      else if (answer > 0)
        approved = true;
      else if (now - pendingLastMs > ESPNOW_PENDING_STALE_MS)
      {
        pending = false;
        approved = false;
        cb.status("Waiting for sender...", "Pick Send on the other unit", "Hold any button to cancel");
      }
      if (approved && now - pendingLastMs > ESPNOW_PENDING_STALE_MS)
      {
        pending = false;
        approved = false;
        cb.status("Waiting for sender...", "Pick Send on the other unit", "Hold any button to cancel");
      }
    }

    // Keep answering END retries for a moment after the verdict went out,
    // then finish.
    if (finalized && resultSent && now - resultSentMs > 1500)
    {
      if (finalStatus == ST_OK)
        result = Result::SUCCESS;
      else
      {
        err = "Image failed MD5 check";
        result = Result::FAILED;
      }
      break;
    }
    // Everything arrived but the sender never sent END: the image is
    // already verified, so go with the verdict.
    if (finalized && !resultSent && now - finalizedMs > 5000)
    {
      if (finalStatus == ST_OK)
        result = Result::SUCCESS;
      else
      {
        err = "Image failed MD5 check";
        result = Result::FAILED;
      }
      break;
    }
    if (active && !finalized && now - lastRxMs > ESPNOW_RECV_STALL_MS)
    {
      Update.end();
      err = "Sender went silent";
      result = Result::FAILED;
      break;
    }

    delay(1);
  }

  radioEnd();
  return result;
}

// =====================================================================
// SENDER
// =====================================================================
enum class Wait { GOT, TIMEOUT, CANCELLED, ABORTED };

// Waits for a packet of `type` from `mac` in `session` (and, if wantSeq >= 0,
// carrying that sequence number). Anything else is discarded.
static Wait waitFor(const Callbacks &cb, uint8_t type, const uint8_t *mac, uint8_t session,
                    int32_t wantSeq, uint32_t timeoutMs, RxPacket *out)
{
  unsigned long t0 = millis();
  do
  {
    RxPacket p;
    while (popPacket(p))
    {
      Header h;
      if (!parseHeader(p, h))
        continue;
      if (memcmp(p.mac, mac, 6) != 0 || h.session != session)
        continue;
      if (h.type == PKT_ABORT)
        return Wait::ABORTED;
      if (h.type != type)
        continue;
      if (wantSeq >= 0 && h.seq != (uint16_t)wantSeq)
        continue;
      if (out)
        *out = p;
      return Wait::GOT;
    }
    if (userCancelled(cb))
      return Wait::CANCELLED;
    delay(1);
  } while (millis() - t0 < timeoutMs);
  return Wait::TIMEOUT;
}

static bool computeSketchMd5(uint32_t total, String &out)
{
  MD5Builder md5;
  md5.begin();
  uint32_t buf[128]; // 512 bytes, word aligned for ESP.flashRead()
  uint32_t off = 0;
  while (off < total)
  {
    uint32_t n = total - off;
    if (n > sizeof(buf))
      n = sizeof(buf);
    if (!ESP.flashRead(off, buf, (n + 3) & ~3u))
      return false;
    md5.add((uint8_t *)buf, (uint16_t)n);
    off += n;
    yield();
  }
  md5.calculate();
  out = md5.toString();
  return true;
}

Result sendFirmware(const Callbacks &cb, String &err)
{
  const uint32_t total = ESP.getSketchSize();
  if (total == 0)
  {
    err = "Can't read own firmware";
    return Result::FAILED;
  }

  cb.status("Preparing...", "Checking own firmware", "Please wait");
  String md5;
  if (!computeSketchMd5(total, md5) || md5.length() != 32)
  {
    err = "Flash read failed";
    return Result::FAILED;
  }

  if (!radioBegin(err))
    return Result::FAILED;

  Result result = Result::FAILED;
  uint8_t session = (uint8_t)(micros() ^ ESP.getCycleCount());
  if (session == 0)
    session = 1;

  // ---- 1. find a receiver ------------------------------------------------
  cb.status("Searching...", "Pick Receive on the other unit", "Hold any button to cancel");

  uint8_t target[6] = {0};
  HelloBody hello;
  bool found = false;
  unsigned long startMs = millis(), lastDiscoverMs = 0;
  VersionBody vb;
  memset(&vb, 0, sizeof(vb));
  strncpy(vb.version, FIRMWARE_VERSION, sizeof(vb.version) - 1);

  while (!found)
  {
    unsigned long now = millis();
    if (userCancelled(cb))
    {
      result = Result::CANCELLED;
      goto done;
    }
    if (now - startMs > ESPNOW_DISCOVER_TIMEOUT_MS)
    {
      err = "No receiver found";
      goto done;
    }
    if (now - lastDiscoverMs >= 250)
    {
      lastDiscoverMs = now;
      sendPacket(BROADCAST_MAC, PKT_DISCOVER, session, 0, &vb, sizeof(vb));
    }
    RxPacket p;
    while (popPacket(p))
    {
      Header h;
      if (!parseHeader(p, h) || h.type != PKT_HELLO || h.session != session)
        continue;
      if (p.len < HDR + sizeof(HelloBody))
        continue;
      memcpy(&hello, p.data + HDR, sizeof(hello));
      memcpy(target, p.mac, 6);
      found = true;
      break;
    }
    delay(1);
  }

  {
    ensurePeer(target);
    hello.version[sizeof(hello.version) - 1] = 0;
    if (hello.freeSpace < total)
    {
      err = "Receiver lacks space";
      goto done;
    }
    cb.status("Found v" + String(hello.version), macToString(target), "Hold any button to cancel");

    // ---- 2. START -> START_ACK ---------------------------------------------
    StartBody sb;
    memset(&sb, 0, sizeof(sb));
    sb.size = total;
    strncpy(sb.version, FIRMWARE_VERSION, sizeof(sb.version) - 1);
    memcpy(sb.md5, md5.c_str(), 32);

    bool acked = false, waitingForUser = false;
    int tries = 0;
    unsigned long startT = millis();
    while (!acked)
    {
      if (!waitingForUser && tries >= 10)
      {
        err = "Receiver didn't accept";
        goto done;
      }
      if (waitingForUser && millis() - startT > ESPNOW_CONFIRM_TIMEOUT_MS)
      {
        sendPacket(target, PKT_ABORT, session, 0);
        err = "Confirm timed out";
        goto done;
      }
      sendPacket(target, PKT_START, session, 0, &sb, sizeof(sb));
      tries++;
      RxPacket ack;
      Wait w = waitFor(cb, PKT_START_ACK, target, session, -1, 300, &ack);
      if (w == Wait::CANCELLED)
      {
        sendPacket(target, PKT_ABORT, session, 0);
        result = Result::CANCELLED;
        goto done;
      }
      if (w == Wait::ABORTED)
      {
        err = "Receiver refused";
        goto done;
      }
      if (w == Wait::GOT)
      {
        uint8_t status = (ack.len > HDR) ? ack.data[HDR] : ST_BAD_REQUEST;
        if (status == ST_OK)
          acked = true;
        else if (status == ST_NEEDS_CONFIRM)
        {
          if (!waitingForUser)
          {
            waitingForUser = true;
            startT = millis();
            cb.status("Receiver has newer/same", "firmware - waiting for OK", "Hold any button to cancel");
          }
          // Pause before asking again. A refusal arrives as ABORT.
          Wait w2 = waitFor(cb, 0, target, session, -1, 400, nullptr);
          if (w2 == Wait::CANCELLED)
          {
            sendPacket(target, PKT_ABORT, session, 0);
            result = Result::CANCELLED;
            goto done;
          }
          if (w2 == Wait::ABORTED)
          {
            err = "Receiver refused";
            goto done;
          }
        }
        else
        {
          err = (status == ST_NO_SPACE) ? "Receiver lacks space" : "Receiver refused";
          goto done;
        }
      }
    }

    // ---- 3. stream the image, one acknowledged chunk at a time ---------------
    cb.status("Sending...", "to " + macToString(target), "Hold any button to cancel");
    int lastPct = -1;
    reportProgress(cb, 0, total, lastPct);

    uint32_t chunk[ESPNOW_CHUNK_BYTES / 4];
    uint32_t sent = 0;
    uint16_t seq = 0;
    while (sent < total)
    {
      uint32_t n = total - sent;
      if (n > ESPNOW_CHUNK_BYTES)
        n = ESPNOW_CHUNK_BYTES;
      // `sent` is always a multiple of ESPNOW_CHUNK_BYTES (a multiple of 4),
      // so the address stays word aligned; the final read rounds up to a
      // whole word and only n bytes are sent.
      if (!ESP.flashRead(sent, chunk, (n + 3) & ~3u))
      {
        sendPacket(target, PKT_ABORT, session, 0);
        err = "Flash read failed";
        goto done;
      }

      bool delivered = false;
      for (int attempt = 0; attempt < ESPNOW_MAX_RETRIES && !delivered; attempt++)
      {
        sendPacket(target, PKT_DATA, session, seq, chunk, n);
        Wait w = waitFor(cb, PKT_ACK, target, session, seq, ESPNOW_ACK_TIMEOUT_MS, nullptr);
        if (w == Wait::GOT)
          delivered = true;
        else if (w == Wait::CANCELLED)
        {
          sendPacket(target, PKT_ABORT, session, 0);
          result = Result::CANCELLED;
          goto done;
        }
        else if (w == Wait::ABORTED)
        {
          err = "Receiver aborted";
          goto done;
        }
      }
      if (!delivered)
      {
        err = "Lost contact with receiver";
        goto done;
      }
      sent += n;
      seq++;
      reportProgress(cb, sent, total, lastPct);
    }

    // ---- 4. END -> RESULT ------------------------------------------------------
    cb.status("Verifying...", "Receiver is checking MD5", "Do not power off");
    for (int attempt = 0; attempt < 20; attempt++)
    {
      sendPacket(target, PKT_END, session, 0);
      RxPacket res;
      Wait w = waitFor(cb, PKT_RESULT, target, session, -1, 400, &res);
      if (w == Wait::CANCELLED)
      {
        // Everything has been delivered; the receiver already decided. Just stop waiting.
        result = Result::CANCELLED;
        goto done;
      }
      if (w == Wait::ABORTED)
      {
        err = "Receiver aborted";
        goto done;
      }
      if (w == Wait::GOT)
      {
        uint8_t status = (res.len > HDR) ? res.data[HDR] : ST_BAD_REQUEST;
        if (status == ST_OK)
          result = Result::SUCCESS;
        else
          err = (status == ST_VERIFY_FAIL) ? "Receiver MD5 mismatch" : "Receiver rejected it";
        goto done;
      }
    }
    err = "No confirmation";
  }

done:
  radioEnd();
  return result;
}

} // namespace EspNowOta