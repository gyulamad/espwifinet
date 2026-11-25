# ESP Wifi Network (base project)

[![CI](https://github.com/gyulamad/espwifinet/actions/workflows/ci.yml/badge.svg)](https://github.com/gyulamad/espwifinet/actions/workflows/ci.yml)

Base architecture for switching a GPIO leg on a fleet of ESP nodes from
one dashboard toggle — the starting point for later home-automation
development (project #3 · Home automation, task #33).

- **Relay** (`nodejs/esp-relay/`, Node.js on a Raspberry Pi): one raw TCP
  hub the nodes dial into (works behind WiFi extenders/NAT), one HTTP API
  for the PHP frontend, OTA firmware serving, per-device remote logs.
- **Frontend** (`php/espdash/`, PHP): an initial setup page that writes
  config.php on first run, login, dashboard with the single On/Off toggle,
  Devices page with every node's connection status + SSID + signal
  strength, Logs, Settings (GPIO pin, reporting cadence, offline window,
  node log level, names, plus the relay's read-only system summary),
  Firmware upload.
- **Node** (`arduino/ESP_node/`, Arduino sketch, one source tree for the
  ESP8266 and the ESP32 / WROOM32 — board.h carries the few board-specific
  bits): multi-network WiFi
  management that always (re)connects to the strongest known network
  (the WiFiMulti pattern), outbound relay connection with
  framed-JSON protocol, status reports (SSID, RSSI, heap, uptime), in-band
  remote logging with repeat suppression and a settings-driven level
  filter, OTA self-update, settings-driven GPIO, and a cable-free first
  install (#135): a factory-new node opens its own `espnode-setup-xxxxxx`
  WiFi where a phone picks the networks; they land in the node's flash and
  override the compiled-in list.
- **Tests** (`./run_tests.sh`): Node unit + e2e (real relay, simulated
  node) + C++ logic tests against stub Arduino headers.

Design decisions worth knowing (each documented in the code):

- The wire protocol is `[4-byte BE length][JSON]` after a one-time
  `<deviceId>\t<apiKey>\n` auth line.
- Commands travel **down the node's own outbound connection**; nodes that
  were offline during a toggle catch up on reconnect because the relay
  re-sends the persisted desired state on every connect.
- WiFi is a LIST, not a value (#36): the node (re)connects to the strongest
  known network and roams only when the current link degrades, so a healthy
  link is never churned. The setup wizard and the settings page gather
  every previously hard-coded value; the relay's ports/keys stay in
  config.js on purpose — they need a restart, so the page shows them
  read-only instead.
- The node's WiFi list has a precedence (#135): the setup page's flash
  store overrides the `config.h` list, and the setup WiFi opens only when
  the node has nothing usable to join with (factory state — serve until
  saved) or joined nothing for a while (fallback — bounded windows, so a
  temporary outage heals itself). The store is validated on every load and
  a damaged one falls back to the compiled list.
- Every module keeps its decision logic in pure functions/classes and the
  IO in thin wrappers, which is what makes the relay unit-testable and the
  sketch desktop-testable without hardware.
- Security baseline: shared secrets for both hops (node→relay, PHP→relay,
  constant-time compared), session login with CSRF tokens and CSP for the
  dashboard, length caps on every wire buffer, HTML escaping via one
  helper, no shell exec anywhere. The node's setup page (task #135) carries
  the same baseline: WPA2-gated AP, per-session CSRF token, strict bounded
  request parsing, security headers, and passwords are never echoed.

See **INSTALL.md** for setup (Pi, PHP, flashing, OTA, troubleshooting) and
**acronym.md** (#50) for every acronym the docs use.
