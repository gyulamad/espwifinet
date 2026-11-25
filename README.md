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
- **Node** (`arduino/ESP_node/`, Arduino sketch, one source tree for every
  supported ESP board — board.h carries the few board-specific
  bits): multi-network WiFi
  management that always (re)connects to the strongest known network
  (the WiFiMulti pattern), outbound relay connection with
  framed-JSON protocol, status reports (SSID, RSSI, heap, uptime), in-band
  remote logging with repeat suppression and a settings-driven level
  filter, OTA self-update, settings-driven GPIO, and a cable-free first
  install (#135): a node whose first WiFi connect has never happened opens
  its own `espnode-setup-xxxxxx` WiFi where a phone picks the networks; the
  flash store is the node's only credential list.
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
- The node carries NO config file at all (#140): the sketch has nothing to
  edit — the WiFi list AND the relay address + fleet key live in its flash
  store, written by the node's own setup page, so a flashed binary plus one
  phone visit is a complete install and the end user never touches source
  code. The store also carries a `provisioned` flag (the first successful
  WiFi connect happened) and a `setup-armed` flag (the dashboard's
  SETUP_MODE maintenance switch). The setup WiFi opens exactly while the
  first install is not complete or setup is armed, and NEVER on a
  provisioned node in an outage: a WiFi outage retries, it does not open a
  captive portal. The store is validated on every load; a damaged one reads
  as "no credentials" (which means the portal, not garbage on the wire).
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
