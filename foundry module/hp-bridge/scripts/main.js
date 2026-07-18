/**
 * HP Bridge
 * ---------
 * Runs client-side inside the GM's Foundry session. Dials OUT to a small
 * self-hosted relay (see relay-server/) over a plain WebSocket. The relay
 * is what actually terminates HTTP connections from the outside world
 * (e.g. an ESP8266) - a browser-sandboxed module cannot open a listening
 * socket itself.
 *
 * Which actors get sent is controlled per-actor by a checkbox this module
 * injects into eligible sheets ("Send to HP Monitor"), stored as an actor
 * flag. Eligible types are characters, NPCs, vehicles, and familiars (see
 * EXPOSABLE_ACTOR_TYPES below) - loot, parties, and other non-combatant
 * types never get the toggle. Only checked actors are ever included in the
 * data sent out. Only two request types exist:
 *   - "list"   -> every checked actor's full hp/maxHp/tempHp/name
 *   - "update" -> set one checked actor's hp/tempHp
 *
 * Only the GM's client maintains the relay connection, since only a GM
 * has unrestricted permission to read/update every actor.
 */

const MODULE_ID = "hp-bridge";
const EXPOSED_FLAG = "exposed";

// Actor types eligible for the "Send to HP Monitor" toggle. Broad enough to
// cover the common combatant types across systems (dnd5e's character/npc/
// vehicle, pf2e's familiar, etc.) without picking up non-combatant types
// like loot or parties.
const EXPOSABLE_ACTOR_TYPES = ["character", "npc", "vehicle", "familiar"];

function log(...args) {
  console.log(`%c[${MODULE_ID}]`, "color:#6cf", ...args);
}
function warn(...args) {
  console.warn(`[${MODULE_ID}]`, ...args);
}




// ---------------------------------------------------------------------
// Relay connection
// ---------------------------------------------------------------------
class HpBridgeConnection {
  constructor() {
    this.ws = null;
    this.reconnectTimer = null;
    this.manuallyClosed = false;
  }

  get relayUrl() {
    return game.settings.get(MODULE_ID, "relayUrl");
  }

  start() {
    this.manuallyClosed = false;
    this._connect();
  }

  stop() {
    this.manuallyClosed = true;
    if (this.reconnectTimer) clearTimeout(this.reconnectTimer);
    if (this.ws) this.ws.close();
  }

  _connect() {
    const url = this.relayUrl;
    if (!url) {
      warn("No relay URL configured, not connecting.");
      return;
    }

    log("Connecting to relay:", url);
    let ws;
    try {
      ws = new WebSocket(url);
    } catch (err) {
      warn("Failed to open WebSocket:", err);
      this._scheduleReconnect();
      return;
    }
    this.ws = ws;

    ws.addEventListener("open", () => {
      log("Connected.");
    });

    ws.addEventListener("message", (evt) => {
      this._handleMessage(evt.data);
    });

    ws.addEventListener("close", (evt) => {
      warn(`Relay connection closed (code ${evt.code}).`);
      this.ws = null;
      if (!this.manuallyClosed) this._scheduleReconnect();
    });

    ws.addEventListener("error", (evt) => {
      warn("WebSocket error:", evt);
    });
  }

  _scheduleReconnect() {
    if (this.reconnectTimer) return;
    const delay = game.settings.get(MODULE_ID, "reconnectDelayMs") || 5000;
    this.reconnectTimer = setTimeout(() => {
      this.reconnectTimer = null;
      if (!this.manuallyClosed) this._connect();
    }, delay);
  }

  async _handleMessage(raw) {
    let msg;
    try {
      msg = JSON.parse(raw);
    } catch (err) {
      warn("Received malformed JSON from relay:", raw);
      return;
    }

    const { reqId, type, payload } = msg;
    if (!reqId || !type) return;

    try {
      let result;
      switch (type) {
        case "list":
          result = { ok: true, characters: this._listCharacters() };
          break;
        case "update":
          result = await this._updateCharacter(payload?.uuid, payload?.hp, payload?.tempHp);
          break;
        default:
          result = { ok: false, error: `unknown request type: ${type}` };
      }
      this._reply(reqId, result);
    } catch (err) {
      warn(`Error handling "${type}" request:`, err);
      this._reply(reqId, { ok: false, error: String(err?.message ?? err) });
    }
  }

  _reply(reqId, result) {
    if (!this.ws || this.ws.readyState !== WebSocket.OPEN) return;
    this.ws.send(JSON.stringify({ reqId, ...result }));
  }

  _canModify(actor) {
    // GMs can modify every actor regardless of assigned ownership, so
    // don't rely solely on actor.isOwner (which reflects the explicit
    // permission grant, not the GM's blanket authority).
    return game.user.isGM || actor.isOwner;
  }

  _exposedActors() {
    return game.actors.contents.filter(a =>
      EXPOSABLE_ACTOR_TYPES.includes(a.type) &&
      a.getFlag(MODULE_ID, EXPOSED_FLAG) === true &&
      this._canModify(a) // must actually be able to update this actor, not just be checked
    );
  }

  _actorToCharacter(actor) {
    const hp = foundry.utils.getProperty(actor, "system.attributes.hp") ?? {};
    return {
      uuid: actor.uuid,
      name: actor.name,
      hp: Number(hp.value ?? 0),
      maxHp: Number(hp.max ?? 0),
      tempHp: Number(hp.temp ?? 0)
    };
  }

  _listCharacters() {
    return this._exposedActors().map(a => this._actorToCharacter(a));
  }

  async _updateCharacter(uuid, hp, tempHp) {
    if (!uuid) return { ok: false, error: "missing uuid" };
    const actor = this._resolveActor(uuid);
    if (!actor) return { ok: false, error: `no actor found for uuid ${uuid}` };
    if (actor.getFlag(MODULE_ID, EXPOSED_FLAG) !== true) {
      // Refuse to update a character that isn't opted in, even if a
      // stale uuid gets sent (e.g. someone unchecked it after the device
      // already had it cached) - keeps the checkbox as the single source
      // of truth for what the device is allowed to touch.
      return { ok: false, error: "actor is not checked for HP Monitor" };
    }
    if (!this._canModify(actor)) {
      // The bridge account doesn't actually have permission to modify
      // this actor (e.g. checked by the GM, but the bridge is now running
      // as a non-GM user without Owner on it) - refuse rather than let
      // Foundry's own permission check throw partway through.
      return { ok: false, error: "bridge account lacks permission to modify this actor" };
    }

    const update = {};
    if (hp !== undefined && hp !== null) update["system.attributes.hp.value"] = Number(hp);
    if (tempHp !== undefined && tempHp !== null) update["system.attributes.hp.temp"] = Number(tempHp);

    if (Object.keys(update).length === 0) {
      return { ok: false, error: "no hp/tempHp values provided" };
    }

    await actor.update(update);
    return { ok: true, character: this._actorToCharacter(actor) };
  }

  _resolveActor(uuid) {
    if (uuid.startsWith("Actor.")) {
      const id = uuid.slice("Actor.".length);
      const actor = game.actors.get(id);
      if (actor) return actor;
    }
    return game.actors.get(uuid) ?? null;
  }
}

const bridge = new HpBridgeConnection();

function isBridgeUser() {
  const configuredName = game.settings.get(MODULE_ID, "bridgeUserName")?.trim();

  // No username configured -> fall back to the original behavior (GM's
  // client maintains the relay link).
  if (!configuredName) return game.user.isGM;

  return game.user.name === configuredName;
}

Hooks.once("ready", () => {
  if (!isBridgeUser()) return; // only the configured account's client maintains the relay link

  if (!game.user.isGM) {
    warn(
      `Logged in as "${game.user.name}", which matches the configured bridge account, but this user is not a GM. ` +
      "Actor updates sent from the relay will fail for any actor this user doesn't have OWNER permission on."
    );
  }

  bridge.start();
});

// Expose for the console / macros, e.g. game.modules.get("hp-bridge").api.reconnect()
Hooks.once("setup", () => {
  const mod = game.modules.get(MODULE_ID);
  if (mod) {
    mod.api = {
      reconnect: () => { bridge.stop(); bridge.start(); },
      stop: () => bridge.stop()
    };
  }
});