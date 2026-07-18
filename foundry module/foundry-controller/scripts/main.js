/**
 * HP Bridge
 * ---------
 * Runs client-side inside the GM's or named character (game.settings.foundry-
 * controller.FCupdaterUserName")'s Foundry session. Dials OUT to a small
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
 * data sent out. Request types:
 *   - "list"   -> every checked actor's full hp/maxHp/tempHp/name
 *   - "update" -> set one checked actor's hp/tempHp
 *   - "move"        -> preview-only: set a target {col, row} grid cell for
 *                      one checked actor's token (col 0 / row 0 = top-left
 *                      of scene) and get back the distance to it, without
 *                      actually moving anything yet - like hovering a drag
 *                      before dropping it. Works on square and hex grids
 *                      via the scene's own Grid API.
 *   - "moveConfirm" -> commit the target set by the last "move" for {uuid}
 *   - "moveCancel"  -> discard the pending target set by "move" for {uuid}
 *   - "circle"      -> drop a circular MeasuredTemplate centered on one
 *                      checked actor's token, e.g. {radius, color, durationMs}
 *

 */

const MODULE_ID = "foundry-controller";
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
class FoundryControllerConnection {
  constructor() {
    this.ws = null;
    this.reconnectTimer = null;
    this.manuallyClosed = false;
    // actor.uuid -> { tokenDoc, targetOffset, point, graphics, timeoutId }
    // One pending preview target per actor, set by "move" and consumed by
    // "moveConfirm" (or discarded by "moveCancel" / a fresh "move").
    this.pendingMoves = new Map();
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
    for (const uuid of Array.from(this.pendingMoves.keys())) this._clearMovePreview(uuid);
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
        case "move":
          result = await this._previewMove(payload?.uuid, payload?.col, payload?.row, payload?.timeoutMs);
          break;
        case "moveConfirm":
          result = await this._confirmMove(payload?.uuid);
          break;
        case "moveCancel":
          result = this._cancelMove(payload?.uuid);
          break;
        case "circle":
          result = await this._createCircle(payload?.uuid, payload?.radius, {
            color: payload?.color,
            durationMs: payload?.durationMs
          });
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

  // Shared by every write-capable action (update/move/circle): resolves the
  // actor and confirms it's both opted in (checkbox) and actually writable
  // by whichever user the bridge is currently logged in as. Returns an
  // { ok:false, error } object if the action should be refused, or null if
  // the actor is authorized and safe to act on.
  _authorizeActorAction(actor) {
    if (!actor) return { ok: false, error: "actor not found" };
    if (actor.getFlag(MODULE_ID, EXPOSED_FLAG) !== true) {
      // Refuse to touch a character that isn't opted in, even if a stale
      // uuid gets sent (e.g. someone unchecked it after the device already
      // had it cached) - keeps the checkbox as the single source of truth
      // for what the device is allowed to touch.
      return { ok: false, error: "actor is not checked for HP Monitor" };
    }
    if (!this._canModify(actor)) {
      // The bridge account doesn't actually have permission to modify this
      // actor (e.g. checked by the GM, but the bridge is now running as a
      // non-GM user without Owner on it) - refuse rather than let Foundry's
      // own permission check throw partway through.
      return { ok: false, error: "bridge account lacks permission to modify this actor" };
    }
    return null;
  }

  // Finds the TokenDocument(s) representing this actor. Uses
  // getActiveTokens(false, true) (document=true) rather than
  // canvas.tokens.placeables so this works even when the bridge client
  // isn't currently viewing the scene the token lives on - moving a token
  // or dropping a template doesn't require it to be rendered on screen.
  // If the actor has multiple placed tokens (duplicates on a scene, or
  // tokens on more than one scene), prefer the one on the world's active
  // scene (game.scenes.active - the scene flagged as "active" for players,
  // not merely whichever one a GM happens to be viewing). Falls back to
  // the first token found if none are on the active scene.
  _resolveActorToken(actor) {
    const tokenDocs = actor.getActiveTokens(false, true);
    if (tokenDocs.length === 0) return null;

    const activeScene = game.scenes?.active;
    if (activeScene) {
      const onActive = tokenDocs.find(t => t.parent?.id === activeScene.id);
      if (onActive) return onActive;
    }
    return tokenDocs[0];
  }

  async _updateCharacter(uuid, hp, tempHp) {
    if (!uuid) return { ok: false, error: "missing uuid" };
    const actor = this._resolveActor(uuid);
    const authError = this._authorizeActorAction(actor);
    if (authError) return authError;

    const update = {};
    if (hp !== undefined && hp !== null) update["system.attributes.hp.value"] = Number(hp);
    if (tempHp !== undefined && tempHp !== null) update["system.attributes.hp.temp"] = Number(tempHp);

    if (Object.keys(update).length === 0) {
      return { ok: false, error: "no hp/tempHp values provided" };
    }

    await actor.update(update);
    return { ok: true, character: this._actorToCharacter(actor) };
  }

  // "Hover" step, mirroring how dragging a token with the mouse works:
  // computes the target grid cell and the distance to it, but does NOT
  // move the token yet. A subsequent "moveConfirm" commits it, or
  // "moveCancel" (or simply sending a new "move") discards it. If only one
  // of col/row is provided, the other axis is left at the token's current
  // position. If the bridge client happens to be viewing the token's
  // scene, this also draws a temporary ruler line + distance label on the
  // canvas, same idea as Foundry's own drag-measurement ruler - though
  // it's a simplified stand-in (a line and a label), not a pixel-identical
  // clone of Foundry's native token-drag ghost.
  async _previewMove(uuid, col, row, timeoutMs) {
    if (!uuid) return { ok: false, error: "missing uuid" };
    if ((col === undefined || col === null) && (row === undefined || row === null)) {
      return { ok: false, error: "no col/row values provided" };
    }

    const actor = this._resolveActor(uuid);
    const authError = this._authorizeActorAction(actor);
    if (authError) return authError;

    const tokenDoc = this._resolveActorToken(actor);
    if (!tokenDoc) return { ok: false, error: "actor has no token placed on any scene" };

    const scene = tokenDoc.parent;
    if (!scene) return { ok: false, error: "token is not attached to a scene" };

    const grid = scene.grid;
    const currentOffset = grid.getOffset({ x: tokenDoc.x, y: tokenDoc.y });
    const targetOffset = {
      i: row !== undefined && row !== null ? Number(row) : currentOffset.i,
      j: col !== undefined && col !== null ? Number(col) : currentOffset.j
    };
    const point = grid.getTopLeftPoint(targetOffset);

    const currentCenter = grid.getCenterPoint(currentOffset);
    const targetCenter = grid.getCenterPoint(targetOffset);

    let distance;
    try {
      distance = grid.measurePath([currentCenter, targetCenter]).distance;
    } catch (err) {
      // Fallback straight-line estimate if measurePath isn't available on
      // this Foundry version's grid class.
      const pixelDist = Math.hypot(targetCenter.x - currentCenter.x, targetCenter.y - currentCenter.y);
      distance = (pixelDist / grid.size) * grid.distance;
    }

    // A fresh preview replaces any earlier one for this actor.
    this._clearMovePreview(actor.uuid);

    const pending = { tokenDoc, targetOffset, point, graphics: [], timeoutId: null };
    this.pendingMoves.set(actor.uuid, pending);

    const effectiveTimeout = Number(timeoutMs) || 15000;
    pending.timeoutId = setTimeout(() => {
      warn(`Move preview for ${actor.name} expired after ${effectiveTimeout}ms without a moveConfirm/moveCancel.`);
      this._clearMovePreview(actor.uuid);
    }, effectiveTimeout);

    if (canvas?.ready && canvas.scene?.id === scene.id) {
      const line = new PIXI.Graphics();
      line.lineStyle(4, 0x00b4ff, 0.85);
      line.moveTo(currentCenter.x, currentCenter.y);
      line.lineTo(targetCenter.x, targetCenter.y);
      line.beginFill(0x00b4ff, 0.35);
      line.drawCircle(targetCenter.x, targetCenter.y, grid.size * 0.4);
      line.endFill();

      const label = new PIXI.Text(
        `${distance.toFixed(1)} ${scene.grid.units ?? ""}`.trim(),
        { fill: 0xffffff, fontSize: 24, stroke: 0x000000, strokeThickness: 4 }
      );
      label.anchor.set(0.5, 1);
      label.position.set((currentCenter.x + targetCenter.x) / 2, (currentCenter.y + targetCenter.y) / 2 - 12);

      canvas.controls.addChild(line);
      canvas.controls.addChild(label);
      pending.graphics.push(line, label);
    }

    return {
      ok: true,
      preview: {
        uuid: tokenDoc.uuid,
        col: targetOffset.j,
        row: targetOffset.i,
        distance,
        units: scene.grid.units ?? null
      }
    };
  }

  // Commits whichever target "move" last set for this actor. Errors if
  // there's no pending preview (e.g. it already expired, was cancelled, or
  // "move" was never called).
  async _confirmMove(uuid) {
    if (!uuid) return { ok: false, error: "missing uuid" };
    const actor = this._resolveActor(uuid);
    const authError = this._authorizeActorAction(actor);
    if (authError) return authError;

    const pending = this.pendingMoves.get(actor.uuid);
    if (!pending) return { ok: false, error: "no pending move target for this actor - send a move first" };

    this._clearMovePreview(actor.uuid);

    await pending.tokenDoc.update({ x: pending.point.x, y: pending.point.y });

    const scene = pending.tokenDoc.parent;
    const finalOffset = scene.grid.getOffset({ x: pending.tokenDoc.x, y: pending.tokenDoc.y });
    return {
      ok: true,
      token: { uuid: pending.tokenDoc.uuid, col: finalOffset.j, row: finalOffset.i }
    };
  }

  // Discards a pending preview target without moving the token.
  _cancelMove(uuid) {
    if (!uuid) return { ok: false, error: "missing uuid" };
    const actor = this._resolveActor(uuid);
    if (!actor) return { ok: false, error: "actor not found" };

    const hadPending = this.pendingMoves.has(actor.uuid);
    this._clearMovePreview(actor.uuid);
    return { ok: true, cleared: hadPending };
  }

  // Removes any preview graphics/timeout for an actor's pending move and
  // deletes its map entry. Safe to call when nothing is pending.
  _clearMovePreview(actorUuid) {
    const pending = this.pendingMoves.get(actorUuid);
    if (!pending) return;
    if (pending.timeoutId) clearTimeout(pending.timeoutId);
    for (const obj of pending.graphics) obj.destroy();
    this.pendingMoves.delete(actorUuid);
  }

  // Drops a circular MeasuredTemplate centered on the actor's token, e.g.
  // for a physical "ping" button. radius is in the scene's distance units
  // (feet/meters/etc, whatever the scene is configured for) - defaults to
  // one grid square's worth if omitted. If durationMs is given, the
  // template deletes itself after that delay instead of persisting.
  async _createCircle(uuid, radius, { color, durationMs } = {}) {
    if (!uuid) return { ok: false, error: "missing uuid" };
    const actor = this._resolveActor(uuid);
    const authError = this._authorizeActorAction(actor);
    if (authError) return authError;

    const tokenDoc = this._resolveActorToken(actor);
    if (!tokenDoc) return { ok: false, error: "actor has no token placed on any scene" };

    const scene = tokenDoc.parent;
    if (!scene) return { ok: false, error: "token is not attached to a scene" };

    const gridSize = scene.grid.size;
    const centerX = tokenDoc.x + (tokenDoc.width * gridSize) / 2;
    const centerY = tokenDoc.y + (tokenDoc.height * gridSize) / 2;

    const templateData = {
      t: "circle",
      x: centerX,
      y: centerY,
      distance: Number(radius) || scene.grid.distance,
      fillColor: color || game.user.color,
      borderColor: "#000000",
      author: game.user.id
    };

    const [created] = await scene.createEmbeddedDocuments("MeasuredTemplate", [templateData]);
    if (!created) return { ok: false, error: "failed to create template" };

    if (durationMs) {
      setTimeout(() => {
        created.delete().catch(err => warn("Failed to auto-delete circle template:", err));
      }, Number(durationMs));
    }

    return { ok: true, templateId: created.id, sceneId: scene.id };
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

const bridge = new FoundryControllerConnection();

function isUpdaterUser() {
  const configuredName = game.settings.get(MODULE_ID, "FCupdaterUserName")?.trim();

  // No username configured -> fall back to the original behavior (GM's
  // client maintains the relay link).
  if (!configuredName) return game.user.isGM;

  return game.user.name === configuredName;
}

Hooks.once("ready", () => {
  if (!isUpdaterUser()) return; // only the configured account's client maintains the relay link

  if (!game.user.isGM) {
    warn(
      `Logged in as "${game.user.name}", which matches the configured updater account, but this user is not a GM. ` +
      "Actor updates sent from the relay will fail for any actor this user doesn't have OWNER permission on."
    );
  }

  bridge.start();
});

// Expose for the console / macros, e.g. game.modules.get("foundry-controller").api.reconnect()
Hooks.once("setup", () => {
  const mod = game.modules.get(MODULE_ID);
  if (mod) {
    mod.api = {
      reconnect: () => { bridge.stop(); bridge.start(); },
      stop: () => bridge.stop()
    };
  }
});