const MODULE_ID = "foundry-controller";
const EXPOSED_FLAG = "exposed";

Hooks.once("init", () => {
  game.settings.register(MODULE_ID, "relayUrl", {
    name: "Relay WebSocket URL",
    hint: "e.g. ws://192.168.1.50:8787/bridge - the address of your self-hosted relay-server.",
    scope: "world",
    config: true,
    type: String,
    default: "ws://localhost:8787/bridge"
  });

  game.settings.register(MODULE_ID, "reconnectDelayMs", {
    name: "Reconnect delay (ms)",
    scope: "world",
    config: true,
    type: Number,
    default: 5000
  });

  game.settings.register(MODULE_ID, "FCupdaterUserName", {
    name: "Updater account username",
    hint: "The relay connection will only start on the client logged in as this Foundry user (case-sensitive, exact match). Leave blank to fall back to the old behavior of using whichever client is logged in as GM.",
    scope: "world",
    config: true,
    type: String,
    default: ""
  });
});