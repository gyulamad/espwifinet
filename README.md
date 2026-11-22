# ESP Wifi Network (base project)

Base architecture for switching a GPIO leg on a fleet of ESP8266 nodes from
one dashboard toggle — the starting point for later home-automation
development (project #3 · Home automation, task #33).

- **Relay** (`nodejs/esp-relay/`, Node.js on a Raspberry Pi): one raw TCP
  hub the nodes dial into (works behind WiFi extenders/NAT), one HTTP API
  for the PHP frontend, OTA firmware serving, per-device remote logs.
- **Frontend** (`php/espdash/`, PHP): login, dashboard with the single
  On/Off toggle, Devices page with every node's connection status + SSID +
  signal strength, Logs, Settings (GPIO pin, names), Firmware upload.
- **Node** (`arduino/ESP8266_Node/`, Arduino sketch): outbound relay
  connection with framed-JSON protocol, status reports (SSID, RSSI, heap,
  uptime), in-band remote logging with repeat suppression, OTA self-update,
  settings-driven GPIO.
- **Tests** (`./run_tests.sh`): Node unit + e2e (real relay, simulated
  node) + C++ logic tests against stub Arduino headers.

Design decisions worth knowing (each documented in the code):

- The wire protocol is `[4-byte BE length][JSON]` after a one-time
  `<deviceId>\t<apiKey>\n` auth line — the same shape as espcamsys2, so the
  patterns (and debugging habits) carry over.
- Commands travel **down the node's own outbound connection**; nodes that
  were offline during a toggle catch up on reconnect because the relay
  re-sends the persisted desired state on every connect.
- Every module keeps its decision logic in pure functions/classes and the
  IO in thin wrappers, which is what makes the relay unit-testable and the
  sketch desktop-testable without hardware.
- Security baseline: shared secrets for both hops (node→relay, PHP→relay,
  constant-time compared), session login with CSRF tokens and CSP for the
  dashboard, length caps on every wire buffer, HTML escaping via one
  helper, no shell exec anywhere.

See **INSTALL.md** for setup (Pi, PHP, flashing, OTA, troubleshooting).
