/**
 * foundry-controller relay
 * ------------------------
 * Sits between:
 *   - the "foundry-controller" Foundry module, which dials IN to us over a
 *     WebSocket (path /bridge), and
 *   - an external HTTP client (e.g. the ESP8266 HP monitor), which talks
 *     to us over plain HTTP.
 *
 * HTTP surface is deliberately tiny:
 *   GET  /characters          -> every character checked "Send to HP
 *                                Monitor" in Foundry, full data, in one
 *                                response.
 *   POST /characters/update   -> update one of those characters' hp/tempHp.
 *   POST /tokens/move         -> preview-only: set a target for a
 *                                character's token (absolute {col, row}
 *                                or relative {dCol, dRow}) and get back
 *                                the distance to it. Doesn't move
 *                                anything yet - mirrors hovering a drag
 *                                before dropping it. Must be followed by
 *                                a call to /tokens/move/confirm to
 *                                actually commit it.
 *   POST /tokens/move/confirm -> commit the target set by the last
 *                                /tokens/move call for a given uuid.
 *   POST /tokens/move/cancel  -> discard the pending target set by
 *                                /tokens/move for a given uuid.
 *   POST /tokens/circle         -> drop a real, persistent circular map
 *                                 template centered on a character's
 *                                 token, optionally self-deleting after
 *                                 durationMs. Implicitly clears any live
 *                                 preview from /tokens/circle/preview.
 *   POST /tokens/circle/preview -> live, local-only preview of a circle's
 *                                 radius - draws (or redraws in place) on
 *                                 whichever client is viewing the token's
 *                                 scene, without touching any real
 *                                 document. Safe to call repeatedly as a
 *                                 radius is dialed in.
 *   POST /tokens/circle/cancel  -> discard the live preview set by
 *                                 /tokens/circle/preview for a given uuid,
 *                                 without creating anything.
 *   GET  /healthz                -> liveness + whether a Foundry world is linked.
 *
 * NOTE: authentication has been removed. Anyone who can reach this
 * server on the network can read/update characters and can connect as
 * the Foundry side of the bridge. Only run this on a network you trust
 * (e.g. your own LAN), not exposed to the open internet.
 *
 * Run with: node server.js
 * Config: edit the CONFIG block below, or set the equivalent env vars.
 */

const http = require("http");
const os = require("os");
const express = require("express");
const { WebSocketServer } = require("ws");
const crypto = require("crypto");

// ---------------------------------------------------------------------
// CONFIG
// ---------------------------------------------------------------------
const CONFIG = {
  PORT: process.env.PORT ? Number(process.env.PORT) : 8787,
  REQUEST_TIMEOUT_MS: 8000
};

// ---------------------------------------------------------------------
// Find the LAN-reachable IPv4 address(es) of this host, for logging.
// Ordered so the address you most likely want (e.g. your router-assigned
// 192.168.x.x on a real NIC) comes first, ahead of Docker/VPN/virtual
// adapters that also show up as "non-internal" but aren't the address
// an ESP8266 on your home network would actually use.
// ---------------------------------------------------------------------
const VIRTUAL_IFACE_HINTS = [
  "docker", "veth", "vmnet", "vboxnet", "virbr", "tailscale",
  "wg", "tun", "utun", "zerotier", "hyper-v", "vethernet"
];

function scoreAddress(address, ifaceName) {
  let score = 0;

  // Prefer the ranges most home/office LANs actually hand out.
  if (/^192\.168\./.test(address)) score += 30;
  else if (/^10\./.test(address)) score += 20;
  else if (/^172\.(1[6-9]|2\d|3[01])\./.test(address)) score += 10; // 172.16-172.31

  // Penalize interfaces that are typically virtual/VPN/container, since
  // those often hand out addresses in the same private ranges too.
  const lowerName = ifaceName.toLowerCase();
  if (VIRTUAL_IFACE_HINTS.some(hint => lowerName.includes(hint))) score -= 50;

  return score;
}

function getLanAddresses() {
  const found = [];
  const interfaces = os.networkInterfaces();
  for (const name of Object.keys(interfaces)) {
    for (const iface of interfaces[name] || []) {
      if (iface.family === "IPv4" && !iface.internal) {
        found.push({ address: iface.address, score: scoreAddress(iface.address, name) });
      }
    }
  }
  found.sort((a, b) => b.score - a.score);
  return found.map(f => f.address);
}

// ---------------------------------------------------------------------
// State: exactly one Foundry world connects at a time.
// ---------------------------------------------------------------------
let foundryConn = null; // { ws, worldTitle, system }
const pending = new Map(); // reqId -> { resolve, reject, timer }

function callFoundry(type, payload) {
  return new Promise((resolve, reject) => {
    if (!foundryConn || foundryConn.ws.readyState !== foundryConn.ws.OPEN) {
      reject(new Error("no Foundry world connected to relay"));
      return;
    }
    const reqId = crypto.randomUUID();
    const timer = setTimeout(() => {
      pending.delete(reqId);
      reject(new Error("timed out waiting for Foundry response"));
    }, CONFIG.REQUEST_TIMEOUT_MS);

    pending.set(reqId, { resolve, reject, timer });
    foundryConn.ws.send(JSON.stringify({ reqId, type, payload }));
  });
}

// ---------------------------------------------------------------------
// WebSocket side: the Foundry module connects here.
// ---------------------------------------------------------------------
const wss = new WebSocketServer({ noServer: true });

wss.on("connection", (ws) => {
  // No handshake/auth step: the first connection becomes the active
  // Foundry link. A "hello" message can still optionally set metadata.
  if (foundryConn && foundryConn.ws !== ws) {
    console.log("[foundry-controller-relay] replacing existing Foundry connection.");
    try { foundryConn.ws.close(4002, "replaced by new connection"); } catch {}
  }
  foundryConn = { ws, worldTitle: "Foundry World", system: "" };
  console.log("[foundry-controller-relay] Foundry world connected.");

  ws.on("message", (raw) => {
    let msg;
    try {
      msg = JSON.parse(raw.toString());
    } catch {
      return;
    }

    if (msg.type === "hello") {
      foundryConn.worldTitle = msg.worldTitle || foundryConn.worldTitle;
      foundryConn.system = msg.system || foundryConn.system;
      console.log(`[foundry-controller-relay] Foundry world identified: "${foundryConn.worldTitle}" (${foundryConn.system})`);
      return;
    }

    // Response to one of our pending requests.
    const { reqId, ...result } = msg;
    const waiter = pending.get(reqId);
    if (!waiter) return;
    pending.delete(reqId);
    clearTimeout(waiter.timer);
    waiter.resolve(result);
  });

  ws.on("close", () => {
    if (foundryConn && foundryConn.ws === ws) {
      console.warn("[foundry-controller-relay] Foundry world disconnected.");
      foundryConn = null;
    }
  });
});

// ---------------------------------------------------------------------
// HTTP side: the ESP8266 (or anything else) talks here.
// ---------------------------------------------------------------------
const app = express();
app.use(express.json());

// One GET, returns every checked character's full data.
// Response: {"characters":[{"uuid","name","hp","maxHp","tempHp"}, ...]}
app.get("/characters", async (req, res) => {
  try {
    const result = await callFoundry("list", {});
    if (!result.ok) {
      res.status(502).json({ error: result.error || "list failed" });
      return;
    }
    res.json({ characters: result.characters || [] });
  } catch (err) {
    res.status(504).json({ error: String(err.message || err) });
  }
});

// One POST, updates a single character.
// Body: {"uuid":"...", "hp":9, "tempHp":0}
app.post("/characters/update", async (req, res) => {
  const { uuid, hp, tempHp } = req.body || {};
  if (!uuid) {
    res.status(400).json({ error: "missing uuid in body" });
    return;
  }
  try {
    const result = await callFoundry("update", { uuid, hp, tempHp });
    if (!result.ok) {
      res.status(502).json({ error: result.error || "update failed" });
      return;
    }
    res.json({ ok: true, character: result.character });
  } catch (err) {
    res.status(504).json({ error: String(err.message || err) });
  }
});

// Preview-only move: sets a target for a character's token and reports
// the distance to it, without moving anything yet. Follow up with
// /tokens/move/confirm to commit, or /tokens/move/cancel to discard.
// Target is either absolute {"col":4,"row":2} or relative {"dCol":1,"dRow":0}
// (an offset from the token's actual current position - safe to resend as
// a running total on every call, e.g. from a joystick that doesn't know
// the token's absolute position). If both are given, absolute wins.
// Body: {"uuid":"...", "col":4, "row":2, "timeoutMs":15000}
// Body: {"uuid":"...", "dCol":1, "dRow":0, "timeoutMs":15000}
app.post("/tokens/move", async (req, res) => {
  const { uuid, col, row, dCol, dRow, timeoutMs } = req.body || {};
  if (!uuid) {
    res.status(400).json({ error: "missing uuid in body" });
    return;
  }
  const hasAbsolute = col !== undefined || row !== undefined;
  const hasRelative = dCol !== undefined || dRow !== undefined;
  if (!hasAbsolute && !hasRelative) {
    res.status(400).json({ error: "must provide col/row or dCol/dRow in body" });
    return;
  }
  try {
    const result = await callFoundry("move", { uuid, col, row, dCol, dRow, timeoutMs });
    if (!result.ok) {
      res.status(502).json({ error: result.error || "move preview failed" });
      return;
    }
    res.json({ ok: true, preview: result.preview });
  } catch (err) {
    res.status(504).json({ error: String(err.message || err) });
  }
});

// Commits whichever target the last /tokens/move call set for this uuid.
// Body: {"uuid":"..."}
app.post("/tokens/move/confirm", async (req, res) => {
  const { uuid } = req.body || {};
  if (!uuid) {
    res.status(400).json({ error: "missing uuid in body" });
    return;
  }
  try {
    const result = await callFoundry("moveConfirm", { uuid });
    if (!result.ok) {
      res.status(502).json({ error: result.error || "move confirm failed" });
      return;
    }
    res.json({ ok: true, token: result.token });
  } catch (err) {
    res.status(504).json({ error: String(err.message || err) });
  }
});

// Discards the pending target set by /tokens/move for this uuid, without
// moving the token.
// Body: {"uuid":"..."}
app.post("/tokens/move/cancel", async (req, res) => {
  const { uuid } = req.body || {};
  if (!uuid) {
    res.status(400).json({ error: "missing uuid in body" });
    return;
  }
  try {
    const result = await callFoundry("moveCancel", { uuid });
    if (!result.ok) {
      res.status(502).json({ error: result.error || "move cancel failed" });
      return;
    }
    res.json({ ok: true, cleared: result.cleared });
  } catch (err) {
    res.status(504).json({ error: String(err.message || err) });
  }
});

// Drops a circular map template centered on a character's token.
// Body: {"uuid":"...", "radius":10, "color":"#ff0000", "durationMs":4000}
app.post("/tokens/circle", async (req, res) => {
  const { uuid, radius, color, durationMs } = req.body || {};
  if (!uuid) {
    res.status(400).json({ error: "missing uuid in body" });
    return;
  }
  try {
    const result = await callFoundry("circle", { uuid, radius, color, durationMs });
    if (!result.ok) {
      res.status(502).json({ error: result.error || "circle failed" });
      return;
    }
    res.json({ ok: true, templateId: result.templateId, sceneId: result.sceneId });
  } catch (err) {
    res.status(504).json({ error: String(err.message || err) });
  }
});

// Live, local-only preview of a circle's radius - draws (or redraws in
// place) on whichever client is viewing the token's scene, without
// touching any real document. Safe to call repeatedly as a radius is
// dialed in (e.g. from a physical controller's circle mode); each call
// replaces the previous preview. Follow up with /tokens/circle to commit
// it as a real, persistent template (which implicitly clears this
// preview), or /tokens/circle/cancel to discard it.
// Body: {"uuid":"...", "radius":10, "color":"#ff0000", "timeoutMs":15000}
app.post("/tokens/circle/preview", async (req, res) => {
  const { uuid, radius, color, timeoutMs } = req.body || {};
  if (!uuid) {
    res.status(400).json({ error: "missing uuid in body" });
    return;
  }
  try {
    const result = await callFoundry("circlePreview", { uuid, radius, color, timeoutMs });
    if (!result.ok) {
      res.status(502).json({ error: result.error || "circle preview failed" });
      return;
    }
    res.json({ ok: true, preview: result.preview });
  } catch (err) {
    res.status(504).json({ error: String(err.message || err) });
  }
});

// Discards the live preview set by /tokens/circle/preview for this uuid,
// without creating anything.
// Body: {"uuid":"..."}
app.post("/tokens/circle/cancel", async (req, res) => {
  const { uuid } = req.body || {};
  if (!uuid) {
    res.status(400).json({ error: "missing uuid in body" });
    return;
  }
  try {
    const result = await callFoundry("circleCancel", { uuid });
    if (!result.ok) {
      res.status(502).json({ error: result.error || "circle cancel failed" });
      return;
    }
    res.json({ ok: true, cleared: result.cleared });
  } catch (err) {
    res.status(504).json({ error: String(err.message || err) });
  }
});

app.get("/healthz", (req, res) => {
  res.json({ ok: true, foundryConnected: !!foundryConn });
});

// ---------------------------------------------------------------------
// Wire up: one HTTP server, WebSocket upgrades on /bridge only.
// ---------------------------------------------------------------------
const server = http.createServer(app);

server.on("upgrade", (req, socket, head) => {
  if (req.url !== "/bridge") {
    socket.destroy();
    return;
  }
  wss.handleUpgrade(req, socket, head, (ws) => {
    wss.emit("connection", ws, req);
  });
});

server.listen(CONFIG.PORT, () => {
  const lanAddrs = getLanAddresses();
  const host = lanAddrs[0] || "localhost";

  console.log(`[foundry-controller-relay] listening on http://${host}:${CONFIG.PORT}`);
  console.log(`[foundry-controller-relay] Foundry module should point at ws://${host}:${CONFIG.PORT}/bridge`);

  if (lanAddrs.length > 1) {
    console.log(`[foundry-controller-relay] other reachable addresses: ${lanAddrs.slice(1).join(", ")}`);
  } else if (lanAddrs.length === 0) {
    console.log("[foundry-controller-relay] no non-internal network interface found; falling back to localhost.");
  }
});