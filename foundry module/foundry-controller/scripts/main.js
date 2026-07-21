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
 * data sent out. Request types:
 *   - "list"   -> every checked actor's full hp/maxHp/tempHp/name
 *   - "update" -> set one checked actor's hp/tempHp
 *   - "move"        -> preview-only: set a target for one checked actor's
 *                      token and get back the distance to it, without
 *                      actually moving anything yet - like hovering a drag
 *                      before dropping it. Target is either absolute
 *                      {col, row} (col 0 / row 0 = top-left of scene) or
 *                      relative {dCol, dRow} (an offset from the token's
 *                      actual current position, safe to resend as a
 *                      running total on every call). Works on square and
 *                      hex grids via the scene's own Grid API. A small
 *                      token (footprint under one full grid square, e.g. a
 *                      Tiny creature in a system that allows sub-square
 *                      tokens) keeps whatever position it currently has
 *                      within its square - it isn't re-centered or
 *                      snapped to a corner when it moves to a new square.
 *                      {timeoutMs} is optional: >0 auto-discards the
 *                      preview after that many ms if never confirmed/
 *                      cancelled; 0 or omitted means no timeout - the
 *                      preview then sits until an explicit "moveConfirm"/
 *                      "moveCancel" (or a fresh "move").
 *   - "moveConfirm" -> commit the target set by the last "move" for {uuid}
 *   - "moveCancel"  -> discard the pending target set by "move" for {uuid}
 *   - "circle"        -> drop a real, persistent circular MeasuredTemplate
 *                        centered on one checked actor's token, e.g.
 *                        {radius, color, durationMs}. Implicitly clears
 *                        any live preview from "circlePreview" below. For
 *                        Large+ tokens (2x2 grid squares or bigger), the
 *                        circle's actual radius is `radius` plus half the
 *                        token's own footprint, so it's measured from the
 *                        outside edge of the token's space rather than its
 *                        center point - matching how most systems define
 *                        range/area origin for creatures bigger than one
 *                        grid square. Medium/Small tokens are unaffected -
 *                        and for a small (sub-square) token specifically,
 *                        the circle is centered on the grid square it
 *                        occupies (not its own, possibly off-center pixel
 *                        position) and measured from that square's edge,
 *                        exactly like a normal Medium creature's. If a
 *                        move is currently queued up for this actor (via
 *                        "move", not yet confirmed/cancelled), the circle
 *                        centers on the move's TARGET cell instead of the
 *                        token's actual position - "move here, then drop
 *                        a circle".
 *   - "circlePreview" -> live, local-only preview of a circle's radius
 *                        centered on one checked actor's token - renders
 *                        via a real (unsaved) MeasuredTemplate object, so
 *                        it looks identical to a confirmed "circle"
 *                        (including the highlighted/hatched grid squares,
 *                        the size/edge-measurement adjustments, and the
 *                        queued-move centering described above),
 *                        without touching any real document. Safe to call
 *                        repeatedly as a radius is dialed in, e.g. from a
 *                        physical controller's circle mode - each call
 *                        replaces the previous preview rather than
 *                        stacking. {radius, color, timeoutMs} - timeoutMs
 *                        follows the same >0/0-or-omitted semantics as
 *                        "move" above.
 *   - "circleCancel"  -> discard the live preview set by "circlePreview"
 *                        for {uuid}, without creating anything
 *
 * Only the GM's client maintains the relay connection, since only a GM
 * has unrestricted permission to read/update every actor.
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
    // "moveConfirm" (or discarded by "moveCancel" / a fresh "move"), or by
    // its own timeout if the caller passed timeoutMs > 0 and then walked
    // away mid-preview. timeoutId is null when no timeout was requested.
    this.pendingMoves = new Map();
    // actor.uuid -> { tokenDoc, radius, graphics, highlightName, timeoutId }
    // One live circle-radius preview per actor, set by "circlePreview" and
    // updated in place by subsequent "circlePreview" calls as the radius
    // changes. Cleared by "circleCancel", by "circle" (the real template
    // creation implicitly removes the preview it's replacing), or by its
    // own timeout - same timeoutId semantics as pendingMoves above.
    this.pendingCirclePreviews = new Map();
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
    for (const uuid of Array.from(this.pendingCirclePreviews.keys())) this._clearCirclePreview(uuid);
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
          result = await this._previewMove(payload?.uuid, payload?.col, payload?.row, payload?.dCol, payload?.dRow, payload?.timeoutMs);
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
        case "circlePreview":
          result = await this._previewCircle(payload?.uuid, payload?.radius, payload?.color, payload?.timeoutMs);
          break;
        case "circleCancel":
          result = this._cancelCirclePreview(payload?.uuid);
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

  // How far a Large+ token's own footprint extends past its center point,
  // in the scene's distance units (feet/meters/etc). Used so that range
  // circles ("circle"/"circlePreview") are measured from the OUTSIDE edge
  // of a Large+ token's space rather than from its center - matching how
  // most systems (D&D5e, PF2e, etc.) define range/area origin for a
  // creature bigger than one grid square. Medium/Small tokens (width and
  // height both <= 1) return 0 here, leaving their existing
  // center-measured behavior untouched, since that already matches a
  // single grid square closely enough not to warrant the adjustment.
  // Uses the larger of width/height so a non-square footprint (e.g. a
  // prone dragon) still gets its circle measured from the outermost edge
  // rather than potentially clipping through the token's art.
  _tokenFootprintRadiusUnits(tokenDoc, grid) {
    const sizeCells = Math.max(tokenDoc.width, tokenDoc.height);
    if (sizeCells <= 1) return 0.5 * grid.distance;
    return (sizeCells / 2) * grid.distance;
  }

  // Origin point (scene pixel coords) to center a range circle on for this
  // token. Normally that's just the token's own pixel center. But a small
  // creature (width or height < 1 grid square) isn't guaranteed to sit at
  // the center of its square - systems that allow sub-square tokens let
  // them be positioned anywhere within it, and move previews now preserve
  // that within-square offset (see _previewMove() below) rather than
  // snapping it to a corner. A circle centered on the token's own pixel
  // position would then drift off-center from the grid and no longer line
  // up with the highlighted squares. So for small tokens we snap to the
  // center of whichever grid square the token currently occupies instead -
  // circles for small creatures are centered, and measured, exactly like a
  // normal Medium creature's (_tokenFootprintRadiusUnits already returns 0
  // for width/height <= 1, so the radius itself needs no separate change).
  _circleOriginPoint(tokenDoc, grid) {
    const gridSize = grid.size;
    const pixelCenterX = tokenDoc.x + (tokenDoc.width * gridSize) / 2;
    const pixelCenterY = tokenDoc.y + (tokenDoc.height * gridSize) / 2;

    if (tokenDoc.width >= 1 && tokenDoc.height >= 1) {
      return { x: pixelCenterX, y: pixelCenterY };
    }

    const offset = grid.getOffset({ x: pixelCenterX, y: pixelCenterY });
    const topLeft = grid.getTopLeftPoint(offset);
    return { x: topLeft.x + gridSize / 2, y: topLeft.y + gridSize / 2 };
  }

  // Origin point for a circle ("circle"/"circlePreview"), accounting for a
  // move that's currently queued up (via "move") but not yet confirmed for
  // this actor. If one exists, the circle centers on the move's TARGET
  // cell instead of the token's actual, not-yet-moved position - so a
  // caller can dial in "move here, then drop a circle" (e.g. from the
  // physical controller's circle mode opened while a move is pending) and
  // see the circle land where the token is about to be, not where it
  // currently is. Falls back to the token's real position when nothing is
  // pending, which is the normal case. Reuses _circleOriginPoint()'s small-
  // token centering logic either way by handing it a stand-in with the
  // right x/y - that logic only ever reads x/y/width/height off what it's
  // given, so a plain object works fine in place of a real TokenDocument.
  _circleOrigin(actor, tokenDoc, grid) {
    const pendingMove = this.pendingMoves.get(actor.uuid);
    const positionSource = pendingMove
      ? { x: pendingMove.point.x, y: pendingMove.point.y, width: tokenDoc.width, height: tokenDoc.height }
      : tokenDoc;
    return this._circleOriginPoint(positionSource, grid);
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
  // "moveCancel" (or simply sending a new "move") discards it. Two ways to
  // specify the target:
  //   - Absolute: {col, row} - a specific grid cell (col 0 / row 0 =
  //     top-left of scene). If only one of col/row is provided, the other
  //     axis is left at the token's current position.
  //   - Relative: {dCol, dRow} - an offset from the token's actual current
  //     position (not from any earlier preview), for callers that don't
  //     know the token's absolute position - e.g. a physical joystick
  //     nudging it one cell at a time. Since this is always computed
  //     against the token's real (unmoved) position, a caller can safely
  //     resend the *total* accumulated offset on every nudge rather than
  //     needing to track a running absolute target itself.
  // If both absolute and relative fields are given, absolute wins.
  // If the bridge client happens to be viewing the token's scene, this
  // also draws a temporary ruler line + distance label on the canvas,
  // same idea as Foundry's own drag-measurement ruler - though it's a
  // simplified stand-in (a line and a label), not a pixel-identical clone
  // of Foundry's native token-drag ghost.
  async _previewMove(uuid, col, row, dCol, dRow, timeoutMs) {
    if (!uuid) return { ok: false, error: "missing uuid" };

    const hasAbsolute = (col !== undefined && col !== null) || (row !== undefined && row !== null);
    const hasRelative = (dCol !== undefined && dCol !== null) || (dRow !== undefined && dRow !== null);
    if (!hasAbsolute && !hasRelative) {
      return { ok: false, error: "no col/row/dCol/dRow values provided" };
    }

    const actor = this._resolveActor(uuid);
    const authError = this._authorizeActorAction(actor);
    if (authError) return authError;

    const tokenDoc = this._resolveActorToken(actor);
    if (!tokenDoc) return { ok: false, error: "actor has no token placed on any scene" };

    const scene = tokenDoc.parent;
    if (!scene) return { ok: false, error: "token is not attached to a scene" };

    const grid = scene.grid;
    // A "small" token (footprint under one full grid square, e.g. a Tiny
    // creature in a system that allows sub-square tokens) isn't necessarily
    // aligned to a cell's top-left corner - it can sit anywhere within its
    // square. For these, use the token's pixel center rather than its raw
    // x/y to figure out which cell it's currently in, since x/y alone could
    // be close enough to a cell boundary to misidentify the square if the
    // token is offset toward one edge.
    const isSmall = tokenDoc.width < 1 || tokenDoc.height < 1;
    const currentPixelPoint = isSmall
      ? { x: tokenDoc.x + (tokenDoc.width * grid.size) / 2, y: tokenDoc.y + (tokenDoc.height * grid.size) / 2 }
      : { x: tokenDoc.x, y: tokenDoc.y };
    // Always the token's real, unmoved position - not any earlier pending
    // preview - so relative offsets stay correct no matter how many times
    // "move" gets called before a confirm/cancel.
    const currentOffset = grid.getOffset(currentPixelPoint);

    let targetOffset;
    if (hasAbsolute) {
      targetOffset = {
        i: row !== undefined && row !== null ? Number(row) : currentOffset.i,
        j: col !== undefined && col !== null ? Number(col) : currentOffset.j
      };
    } else {
      targetOffset = {
        i: currentOffset.i + (Number(dRow) || 0),
        j: currentOffset.j + (Number(dCol) || 0)
      };
    }

    // Full-size (Medium+) tokens snap to the target cell's top-left corner,
    // same as before. A small token instead keeps whatever offset it
    // currently has within its own square (e.g. parked near a corner or
    // edge rather than centered) and carries that same offset over to the
    // target square, so it doesn't visually re-center itself on every move.
    let point;
    if (isSmall) {
      const currentCellTopLeft = grid.getTopLeftPoint(currentOffset);
      const withinCellX = tokenDoc.x - currentCellTopLeft.x;
      const withinCellY = tokenDoc.y - currentCellTopLeft.y;
      const targetCellTopLeft = grid.getTopLeftPoint(targetOffset);
      point = { x: targetCellTopLeft.x + withinCellX, y: targetCellTopLeft.y + withinCellY };
    } else {
      point = grid.getTopLeftPoint(targetOffset);
    }

    // Distance (and the ruler line/label drawn below) is measured between
    // the token's actual center - not just the top-left cell's center - so
    // a multi-cell token (Large/Huge/etc, tokenDoc.width/height > 1) reports
    // and draws correctly instead of being off by half a cell in each
    // direction. Mirrors how _previewCircle()/_createCircle() already find
    // a token's center. `point` itself (used to actually move the token in
    // _confirmMove()) doesn't need any further size adjustment here - for a
    // full-size token, moving its top-left corner by N cells preserves its
    // footprint regardless of size; for a small token, `point` already has
    // its within-cell offset preserved (see above).
    const halfWidthPx = (tokenDoc.width * grid.size) / 2;
    const halfHeightPx = (tokenDoc.height * grid.size) / 2;
    const currentCenter = { x: tokenDoc.x + halfWidthPx, y: tokenDoc.y + halfHeightPx };
    const targetCenter = { x: point.x + halfWidthPx, y: point.y + halfHeightPx };

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

    // timeoutMs > 0 schedules an auto-discard after that many ms if nobody
    // moveConfirm()s/moveCancel()s in the meantime. timeoutMs === 0 (the
    // ESP8266 client's default) means "no timeout" - the preview then sits
    // on the map indefinitely until explicitly confirmed/cancelled, or
    // replaced by a fresh "move".
    const effectiveTimeout = Number(timeoutMs) || 0;
    if (effectiveTimeout > 0) {
      pending.timeoutId = setTimeout(() => {
        warn(`Move preview for ${actor.name} expired after ${effectiveTimeout}ms without a moveConfirm/moveCancel.`);
        this._clearMovePreview(actor.uuid);
      }, effectiveTimeout);
    }

    if (canvas?.ready && canvas.scene?.id === scene.id) {
      const line = new PIXI.Graphics();
      line.lineStyle(4, 0x00b4ff, 0.85);
      line.moveTo(currentCenter.x, currentCenter.y);
      line.lineTo(targetCenter.x, targetCenter.y);

      // Marker at the target is sized to the token's actual footprint
      // (halfWidthPx/halfHeightPx, already computed above for the distance
      // calculation) rather than a fixed dot, so a Large/Huge/etc token's
      // preview shows the space it will actually occupy at the target
      // instead of a small circle that undersells its true footprint.
      const markerWidth = halfWidthPx * 2;
      const markerHeight = halfHeightPx * 2;
      const cornerRadius = Math.min(markerWidth, markerHeight) * 0.15;
      line.beginFill(0x00b4ff, 0.35);
      line.lineStyle(3, 0x00b4ff, 0.9);
      line.drawRoundedRect(
        targetCenter.x - halfWidthPx,
        targetCenter.y - halfHeightPx,
        markerWidth,
        markerHeight,
        cornerRadius
      );
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

    const grid = scene.grid;
    const { x: centerX, y: centerY } = this._circleOrigin(actor, tokenDoc, grid);

    // Requested radius plus (for Large+ tokens) however far the token's own
    // footprint extends past its center - see _tokenFootprintRadiusUnits().
    // Foundry's MeasuredTemplate renders/highlights a circle's "distance" as
    // a radius from (x, y), so bumping this one field is enough to move the
    // rendered edge, border, and grid highlight out to the token's edge -
    // no separate adjustment needed elsewhere for the confirmed template.
    const requestedDistance = Number(radius) || scene.grid.distance;
    const footprintRadius = this._tokenFootprintRadiusUnits(tokenDoc, grid);
    const totalDistance = requestedDistance + footprintRadius;

    const templateData = {
      t: "circle",
      x: centerX,
      y: centerY,
      distance: totalDistance,
      fillColor: color || game.user.color,
      borderColor: "#000000",
      author: game.user.id
    };

    // Replace whatever live preview (see _previewCircle()) this real,
    // persistent template is about to stand in for - otherwise both would
    // be visible on top of each other for a moment.
    this._clearCirclePreview(actor.uuid);

    const [created] = await scene.createEmbeddedDocuments("MeasuredTemplate", [templateData]);
    if (!created) return { ok: false, error: "failed to create template" };

    if (durationMs) {
      setTimeout(() => {
        created.delete().catch(err => warn("Failed to auto-delete circle template:", err));
      }, Number(durationMs));
    }

    return {
      ok: true,
      templateId: created.id,
      sceneId: scene.id,
      radius: requestedDistance,
      effectiveRadius: totalDistance
    };
  }

  // Live, local-only preview of a circle's radius centered on the actor's
  // token - highlights the covered grid squares using the same low-level
  // canvas.interface.grid.highlightPosition() API a real MeasuredTemplate
  // uses internally for its own grid highlight (see _highlightCircleGrid()
  // below), giving the same "hatched squares" look as a confirmed circle
  // (_createCircle) without touching any real document.
  //
  // This intentionally does NOT instantiate a MeasuredTemplate placeable
  // (new CONFIG.MeasuredTemplate.documentClass/objectClass) the way an
  // earlier version of this method did - that crashed with "Cannot read
  // properties of undefined (reading 'getBounds')" inside
  // MeasuredTemplate#_getGridHighlightPositions(), because that internal
  // method assumes the template went through Foundry's normal create/
  // embed lifecycle, which a never-persisted preview object never does.
  // A RegionDocument-based approach was also considered, but Regions are
  // a heavier, persistence-oriented document with the same category of
  // assumption (and Foundry's Grid API has changed enough across recent
  // versions - see the V12 Grid API rework - that leaning on either
  // placeable's internal rendering is fragile). Driving the grid highlight
  // layer directly is the smaller, documented surface both of those
  // ultimately bottom out in anyway.
  //
  // A fresh call for the same actor updates the existing preview's radius
  // rather than stacking a new one, so this is safe (and expected) to call
  // on every encoder tick while someone dials a radius in - e.g. from a
  // physical controller's circle mode. Call _createCircle separately to
  // actually commit it as a real, persistent template (which also clears
  // this preview automatically - see above).
  async _previewCircle(uuid, radius, color, timeoutMs) {
    if (!uuid) return { ok: false, error: "missing uuid" };
    const actor = this._resolveActor(uuid);
    const authError = this._authorizeActorAction(actor);
    if (authError) return authError;

    const tokenDoc = this._resolveActorToken(actor);
    if (!tokenDoc) return { ok: false, error: "actor has no token placed on any scene" };

    const scene = tokenDoc.parent;
    if (!scene) return { ok: false, error: "token is not attached to a scene" };

    const grid = scene.grid;
    const gridSize = grid.size;
    const gridDistance = grid.distance;
    const { x: centerX, y: centerY } = this._circleOrigin(actor, tokenDoc, grid);
    const distanceUnits = Number(radius) || gridDistance;
    // Same edge-measurement adjustment as _createCircle() above, so the
    // preview's highlighted squares land in exactly the same place the
    // confirmed template will use.
    const footprintRadius = this._tokenFootprintRadiusUnits(tokenDoc, grid);
    const totalDistanceUnits = distanceUnits + footprintRadius;
    const pixelRadius = (totalDistanceUnits / gridDistance) * gridSize;

    // A fresh preview call replaces (rather than stacks on top of) any
    // earlier one for this actor - same pattern as move's preview.
    this._clearCirclePreview(actor.uuid);

    const highlightName = `${MODULE_ID}.circlePreview.${actor.uuid}`;
    const pending = { tokenDoc, radius: distanceUnits, graphics: [], highlightName, timeoutId: null };
    this.pendingCirclePreviews.set(actor.uuid, pending);

    // Same timeoutMs semantics as _previewMove() above: >0 schedules an
    // auto-discard after that many ms, 0 (the ESP8266 client's default)
    // means no timeout.
    const effectiveTimeout = Number(timeoutMs) || 0;
    if (effectiveTimeout > 0) {
      pending.timeoutId = setTimeout(() => {
        warn(`Circle preview for ${actor.name} expired after ${effectiveTimeout}ms without a circle/circleCancel.`);
        this._clearCirclePreview(actor.uuid);
      }, effectiveTimeout);
    }

    if (canvas?.ready && canvas.scene?.id === scene.id && canvas.interface?.grid) {
      this._highlightCircleGrid(highlightName, grid, centerX, centerY, pixelRadius, color || game.user.color);
    }

    return {
      ok: true,
      preview: {
        radius: distanceUnits,
        effectiveRadius: totalDistanceUnits,
        units: scene.grid.units ?? null,
        sceneId: scene.id
      }
    };
  }

  // Highlights every grid square whose center point falls within the given
  // circle (center + pixelRadius, both in scene pixel coordinates), using
  // canvas.interface.grid's named-highlight-layer API - the same primitive
  // MeasuredTemplate itself calls into for a real circle's grid highlight.
  // Sweeps the circle's pixel bounding box in grid-offset space rather than
  // pixel space, so this works the same way on square and hex grids (same
  // getOffset/getCenterPoint/getTopLeftPoint calls _previewMove() above
  // already relies on for both grid types).
  _highlightCircleGrid(name, grid, centerX, centerY, pixelRadius, color) {
    const gridHighlight = canvas.interface.grid;
    gridHighlight.addHighlightLayer(name);
    gridHighlight.clearHighlightLayer(name);

    const topLeftOffset = grid.getOffset({ x: centerX - pixelRadius, y: centerY - pixelRadius });
    const bottomRightOffset = grid.getOffset({ x: centerX + pixelRadius, y: centerY + pixelRadius });
    const radiusSquared = pixelRadius * pixelRadius;

    for (let i = topLeftOffset.i; i <= bottomRightOffset.i; i++) {
      for (let j = topLeftOffset.j; j <= bottomRightOffset.j; j++) {
        const offset = { i, j };
        const cellCenter = grid.getCenterPoint(offset);
        const dx = cellCenter.x - centerX;
        const dy = cellCenter.y - centerY;
        if (dx * dx + dy * dy > radiusSquared) continue;

        const topLeft = grid.getTopLeftPoint(offset);
        gridHighlight.highlightPosition(name, { x: topLeft.x, y: topLeft.y, color, alpha: 0.5 });
      }
    }
  }

  // Discards the live preview set by _previewCircle() for this actor,
  // without creating anything. Mirrors _cancelMove().
  _cancelCirclePreview(uuid) {
    if (!uuid) return { ok: false, error: "missing uuid" };
    const actor = this._resolveActor(uuid);
    if (!actor) return { ok: false, error: "actor not found" };

    const hadPending = this.pendingCirclePreviews.has(actor.uuid);
    this._clearCirclePreview(actor.uuid);
    return { ok: true, cleared: hadPending };
  }

  // Removes any preview graphic/timeout for an actor's live circle preview
  // and deletes its map entry. Safe to call when nothing is pending.
  // Mirrors _clearMovePreview().
  _clearCirclePreview(actorUuid) {
    const pending = this.pendingCirclePreviews.get(actorUuid);
    if (!pending) return;
    if (pending.timeoutId) clearTimeout(pending.timeoutId);
    if (pending.highlightName && canvas?.interface?.grid) {
      canvas.interface.grid.destroyHighlightLayer(pending.highlightName);
    }
    for (const obj of pending.graphics) obj.destroy();
    this.pendingCirclePreviews.delete(actorUuid);
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