#pragma once
#include <Arduino.h>
#include "Types.h"

// Thin wrapper around the self-hosted "foundry-controller" relay's HTTP
// surface (see foundry-controller-module/ in this project), not a generic
// Foundry REST API. Endpoint paths and JSON field names are all
// centralized here - if your relay version differs, this is the only
// file you should need to touch.
namespace RelayClient {

  // Result of a previewMove() call: the target cell the relay computed
  // (echoed back in case one axis was left unspecified) and the distance
  // to it in the scene's own units, as measured by Foundry's grid - not
  // moved yet, just previewed.
  struct MovePreviewResult {
    int col = 0;
    int row = 0;
    float distance = 0;
    String units;
  };

  // Result of a confirmMove() call: the token's actual final position.
  struct MoveResult {
    int col = 0;
    int row = 0;
  };

  // Runtime toggle for the in-RAM mock party vs the real relay's HTTP
  // surface ("Local Relay Test Mode" on the settings screen) - defaults to
  // RELAY_TEST_MODE from config.h. Changing it takes effect the next time
  // you (re)connect (begin()+connect()), not mid-connection.
  void setTestMode(bool enabled);
  bool testMode();

  void begin(const String &host, uint16_t port);

  // Quick reachability + "is a Foundry world actually connected" check.

  bool connect();

  // true once connect() has succeeded and a clientId is cached.
  bool isConnected();

  // Fetches all player-character actors (type == ACTOR_TYPE_FILTER).
  // Returns true on success and fills `out`.
  bool fetchCharacters(std::vector<Character> &out);

  // Pushes a new hp value (and temp hp) to the server for the given actor.
  bool updateCharacterHp(const String &uuid, int newHp, int newTempHp);

  // Sets a target grid cell (col, row) for a character's token and reports
  // the distance to it - mirrors hovering a drag before dropping it.
  // Nothing moves yet. Follow up with confirmMove() to commit it, or
  // cancelMove() to discard it. timeoutMs is optional: 0 (the default)
  // means no timeout - the preview then stays on the map indefinitely
  // until confirmed/cancelled. Pass a nonzero value to have the relay
  // auto-discard the pending target after that many ms if it's never
  // confirmed.
  bool previewMove(const String &uuid, int col, int row, MovePreviewResult &out, uint32_t timeoutMs = 0);

  // Same idea as previewMove(), but the target is expressed as an offset
  // (dCol, dRow) from the token's actual current position rather than an
  // absolute cell - for callers that don't know the token's absolute
  // position, like a joystick nudging it one cell at a time. The relay
  // always computes this against the token's real (unmoved) position, so
  // it's safe (and expected) to resend the *total* accumulated offset on
  // every nudge rather than tracking a running absolute target yourself.
  // Same timeoutMs semantics as previewMove() above (0 = no timeout).
  bool previewMoveRelative(const String &uuid, int dCol, int dRow, MovePreviewResult &out, uint32_t timeoutMs = 0);

  // Commits whichever target the last previewMove() call set for this
  // uuid. Fails if there's no pending target (never previewed, already
  // confirmed/cancelled, or it expired).
  bool confirmMove(const String &uuid, MoveResult &out);

  // Discards the pending target set by previewMove() for this uuid,
  // without moving the token. outCleared (if provided) reports whether
  // there was actually a pending target to discard.
  bool cancelMove(const String &uuid, bool *outCleared = nullptr);

  // Drops a circular map template centered on a character's token. radius
  // is in the scene's own distance units (0 = relay's default, one grid
  // square). color is a CSS hex string like "#ff0000" (empty = relay's
  // default). durationMs > 0 makes the template self-delete after that
  // delay instead of persisting. outTemplateId (if provided) receives the
  // created template's id. Implicitly clears any live preview from
  // previewCircle() below - no need to call cancelCirclePreview() first.
  bool createCircle(const String &uuid, float radius = 0, const String &color = "",
                     uint32_t durationMs = 0, String *outTemplateId = nullptr);

  // Live, local-only preview of a circle's radius centered on a
  // character's token - renders identically to a confirmed circle
  // (createCircle(), including the highlighted/hatched grid squares) on
  // whichever relay-side client is viewing the token's scene, without
  // touching any real document. Safe (and expected) to call repeatedly as
  // a radius is dialed in, e.g. from a physical controller's circle mode -
  // each call replaces the previous preview rather than stacking. Follow
  // up with createCircle() to commit it as a real, persistent template, or
  // cancelCirclePreview() to discard it. timeoutMs is optional: 0 (the
  // default) means no timeout - the preview then stays on the map
  // indefinitely until committed/discarded. Pass a nonzero value to have
  // the relay auto-discard the preview after that many ms if never
  // followed up on.
  bool previewCircle(const String &uuid, float radius = 0, const String &color = "", uint32_t timeoutMs = 0);

  // Discards the live preview set by previewCircle() for this uuid,
  // without creating anything. outCleared (if provided) reports whether
  // there was actually a preview to discard.
  bool cancelCirclePreview(const String &uuid, bool *outCleared = nullptr);

  String lastError();


}