# hp-bridge

Self-hosted replacement for the ThreeHats `foundryvtt-rest-api-relay`
dependency, tailored to exactly one job: expose a hand-picked set of
characters' HP to an external device (an ESP8266 HP monitor), and let
that device push HP changes back.

Three pieces:

- **`hp-bridge/`** – a Foundry VTT module. Install it into your Foundry
  world (copy the folder into `Data/modules/hp-bridge`, or zip it and use
  "Install Module" with a manifest URL if you host it somewhere). It adds
  a **"Send to HP Monitor" checkbox** to every character sheet, and dials
  *out* to the relay below over a WebSocket.
- **`relay-server/`** – a small Node/Express process you run yourself,
  anywhere on your network (the same machine as Foundry is simplest). It
  terminates the WebSocket from the Foundry module on one side and plain
  HTTP from the ESP8266 on the other.
- **`RelayClient.cpp`** – the updated firmware file (drop-in replacement
  for the one in your ESP8266 project) that speaks this relay's protocol.

Foundry modules execute inside the browser sandbox and can't open a
listening socket, so `relay-server` is what actually terminates the
ESP8266's HTTP connections.

## Which characters are sent

Open any character sheet - a **"Send to HP Monitor"** checkbox now sits at
the bottom. Check it and that character's name/hp/maxHp/tempHp is included
in every `GET /characters` response; uncheck it and it disappears from
the response entirely (and the relay/module will refuse to update it even
if a stale uuid comes in). Anyone who can edit the sheet (owner or GM)
can toggle it - there's no separate settings-menu allowlist to keep in
sync.

## Setup

### 1. Relay server

```bash
cd relay-server
npm install
npm start
```

Leave this running (e.g. under `pm2`, a systemd service, or just a
terminal) on a machine your ESP8266 and your Foundry instance can both
reach.

### 2. Foundry module

Copy `hp-bridge/` into your Foundry `Data/modules/` folder, then enable it
for your world (Manage Modules). Open its settings and set:

- **Relay WebSocket URL**: `ws://<relay-host>:8787/bridge`

Only the GM's client maintains the connection — log in as GM for it to
connect. Check the browser console (F12) for `[hp-bridge]` log lines to
confirm it connected, then go check the box on whichever characters
you want visible.

### 3. ESP8266 firmware

Drop the included `RelayClient.cpp` in over your existing one. Then:

- Set `RELAY_TEST_MODE` to `0` (build flag, or edit the `#define` in the
  file) to use the real relay instead of the in-RAM mock party.
- Point the device's IP-entry screen at the relay's host and port
  (default `8787`).



## HTTP protocol (ESP8266 ↔ relay)

```
GET /characters
  200 -> {"characters":[{"uuid":"Actor.abc123","name":"Thalric","hp":10,"maxHp":20,"tempHp":0}, ...]}

POST /characters/update
  headers: Content-Type: application/json
  body: {"uuid":"Actor.abc123","hp":9,"tempHp":0}
  200 -> {"ok":true,"character":{...}}

GET /healthz
  200 -> {"ok":true,"foundryConnected":true}
```

## Notes

- Updates are only accepted for characters currently checked "Send to HP
  Monitor" — the module re-checks the flag on every update, not just at
  list time, so unchecking a character on its sheet immediately blocks
  further writes to it even if the device has it cached.
