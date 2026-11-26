# ESP Wifi Network — base project

A small, extensible base for switching GPIOs on ESP boards from a
dashboard:

```
 browser ──► PHP dashboard (Apache/PHP on the Pi)
                │  HTTP + X-API-Key (shared secret)
                ▼
            Node.js relay (the Raspberry Pi)          firmware .bin
                ▲                        ▲                │
   outbound TCP │                        │ HTTP GET       │ upload via dashboard
   (auth line + │                        │ /firmware/*    ▼
   framed JSON) │                 ESP nodes     ◄── OTA self-update
                └────── ESP nodes ────────┘
        (behind WiFi extenders / NAT routers: no inbound port needed)
```

Each ESP node keeps **one outbound TCP connection** to the relay. That is
the key architectural decision (the push connection):
the node dials the relay, so nodes work unchanged behind WiFi extenders and
NAT routers, and the same established connection carries the relay's
commands back down (GPIO toggle, settings, OTA trigger).

Directory layout:

| path | what it is |
| --- | --- |
| `nodejs/esp-relay/` | the relay: raw TCP hub for nodes + HTTP API for the dashboard |
| `php/espdash/` | the PHP frontend: login, dashboard (one toggle), devices, logs, settings, firmware |
| `arduino/ESP_node/` | the node sketch: connection, status reporting, remote logging, OTA, GPIO |
| `tests/` | unit + e2e (Node) and logic (C++) tests; `./run_tests.sh` runs everything |

---

## 1. Install the relay (Raspberry Pi)

Requires Node.js ≥ 18 (`node --version`) on the Pi.

```bash
# on the Pi - start from the code itself (this section assumes your user
# is in the sudo group; `id -un` shows the name used below):
git clone https://github.com/gyulamad/espwifinet
cd espwifinet

# copy the relay into /opt and hand it to YOUR user: the relay process
# runs as your user and WRITES there (settings.json, its log files) - a
# root-owned tree is what made `npm install` fail with EACCES.
sudo mkdir -p /opt/espwifinet
sudo cp -r nodejs/esp-relay /opt/espwifinet/
sudo chown -R "$USER:" /opt/espwifinet/esp-relay
cd /opt/espwifinet/esp-relay

npm install --omit=dev

# 1) create your config
cp example.config.js config.js
nano config.js        # set relayKey and nodeKey!

# generate strong keys (run twice, one per key):
node -e "console.log(require('crypto').randomBytes(24).toString('hex'))"

# 2) try it in the foreground first
node server.js
# -> [relay] node TCP server on :7777
# -> [relay] http api on :8080
#    (health check: curl http://127.0.0.1:8080/healthz)
```

Then make it a service that survives reboots
(`/etc/systemd/system/esp-relay.service`):

```ini
[Unit]
Description=ESP Wifi Network relay
After=network-online.target

[Service]
WorkingDirectory=/opt/espwifinet/esp-relay
ExecStart=/usr/bin/node server.js
Restart=always
User=<your login user>   # <- from `id -un`, NOT root: the relay writes its settings and logs here

[Install]
WantedBy=multi-user.target
```

```bash
sudo systemctl daemon-reload
sudo systemctl enable --now esp-relay
```

Data written by the relay (all inside the relay folder, add to your backups):

- `data/settings.json` — GPIO pin, active level, status interval, offline
  window, node log level, node names (all editable on the settings page)
- `data/state.json` — the dashboard toggle's desired state
- `logs/<device>.log` — one JSON-lines file per node + `relay.log`
- `firmware/` — the currently served OTA image + version + md5

## 2. Install the PHP dashboard

Requires PHP ≥ 8 with the `curl` extension. On Debian/Raspberry Pi OS:

```bash
sudo apt install php php-curl     # the PHP web module + the curl extension
sudo systemctl restart apache2    # loads the extension into the web server
```

Without the extension the dashboard shows a clean reminder page with these
same commands instead of a fatal error.

```bash
sudo cp -r php/espdash /var/www/html/espdash
# the setup page writes config.php INTO this folder (temp file + rename),
# so the WEB SERVER user needs write access to the folder itself - a
# root-owned folder makes setup.php fail with "could not create a
# temporary file":
sudo chown www-data:www-data /var/www/html/espdash
```

Then open `http://<pi-ip>/espdash/setup.php` — the **initial setup page**, the safe-credentials bootstrap. It asks for the
relay URL, the relay key (checked against the live relay before anything is
saved) and the first dashboard login, and writes `config.php` for you. The
page refuses unsafe values (short passwords, the example `CHANGE-ME` keys)
and locks itself once a valid `config.php` exists — it only ever runs before
the first login exists.

Prefer doing it by hand instead of the setup page? Run the copy through
sudo — the folder belongs to the web server user (see the chown above), so
your own login cannot write into it:

```bash
cd /var/www/html/espdash
sudo cp config.example.php config.php
sudo chown www-data:www-data config.php && sudo chmod 640 config.php
sudo nano config.php
#   relay_url : http://127.0.0.1:8080 (the relay you installed above)
#   relay_key : the SAME key as relayKey in the relay's config.js
#   users     : generate a password hash and paste it:
php -r "echo password_hash('YourSecretPassword', PASSWORD_DEFAULT);"
```

The chown/chmod pair matches what the setup page writes: `config.php` ends
up owned by `www-data` — readable by the web server, hidden from the other
logins on the Pi.

Open `http://<pi-ip>/espdash/`, sign in, and you are done. The dashboard is
used over plain HTTP on a home LAN; put it behind TLS (or an SSH tunnel)
before exposing it anywhere beyond that.

## 3. Flash a node

In the Arduino IDE (or arduino-cli):

1. Install the **core for your board** (Boards Manager URL:
   `https://arduino.esp8266.com/stable/package_esp8266com_index.json`).
2. Open `arduino/ESP_node/ESP_node.ino` — ONE sketch for every supported
   ESP board: each board's core defines its own board macro and
   `board.h` picks the includes, the chip id, the watchdog feed and the GPIO
   ceiling from it. No per-board code to maintain.
3. There is NOTHING to edit: the sketch carries no config
   file. The node's WiFi network(s) AND its relay address + fleet key are
   entered on the node's own setup page after the flash (section 4) and
   stored in the node's flash; the device id derives from the chip id.
4. Board "NodeMCU 1.0 (ESP-12E module)" (or your board), Upload.
5. Open the Serial Monitor (115200) — the node logs its boot, WiFi join and
   relay connection there, and every log line also appears on the
   dashboard's Logs page.

**The first flash goes over a cable; every later update goes over the
air.** An OTA update replaces the sketch the node is already running, so a
node needs exactly ONE wired upload — this one — to receive its first
image; from then on the dashboard's Firmware page updates it together with
the rest of the fleet (section 5). To upload without opening the IDE, from
the repository root (the port is `/dev/ttyUSB0` on Linux, `COM3`-style on
Windows — take yours from the IDE's port list):

ESP8266 boards (NodeMCU, Wemos):

```bash
arduino-cli compile --fqbn esp8266:esp8266:nodemcuv2 \
    --output-dir ./build arduino/ESP_node
arduino-cli upload  --fqbn esp8266:esp8266:nodemcuv2 \
    -p /dev/ttyUSB0 --input-dir ./build arduino/ESP_node
```

ESP32 boards (WROOM32 and friends):

```bash
arduino-cli compile --fqbn esp32:esp32:esp32 \
    --output-dir ./build arduino/ESP_node
arduino-cli upload  --fqbn esp32:esp32:esp32 \
    -p /dev/ttyUSB0 --input-dir ./build arduino/ESP_node
```

Both commands handle the board's flash layout themselves (a bare
`write_flash` of the app binary is not enough on a blank ESP32 — bootloader
and partition table have to land too), which is why uploads should go
through the IDE or arduino-cli. The compiled image is the same one the
dashboard serves for OTA (section 5), so what you flash by cable and what
the fleet pulls over the air stay identical.

**Start a node over (erase the flash):** the flash store survives ordinary
reflashes BY DESIGN — that is what keeps a working node working across
updates, so the IDE's normal Upload never clears it. To return a node to
factory state (credentials you can no longer fix from the dashboard, a
store you suspect is corrupted, or a node moving to a new site):

```bash
esptool.py --port /dev/ttyUSB0 erase_flash
```

then upload the sketch again (the commands above) — the node boots blank,
its setup WiFi opens and serves until the form saves. In the Arduino IDE
the same happens in one pass: Tools → Erase Flash → **"All Flash
Contents"**, then Upload. What erase clears: the WiFi list, the relay
address + fleet key and both life-cycle flags (`provisioned`,
`setup-armed`) — the node forgets everything and introduces itself as a
fresh `espnode-setup-…` network. What it does not touch: the dashboard's
history for that device — the device id derives from the chip id, so the
node comes back under the same name with its logs intact.

**Multi-wifi:** the node knows a LIST of networks and always
(re)connects to the strongest one it can see — the WiFiMulti pattern. On
the setup page, "Add another network" adds a row per network (different
floors, rooms or extenders); every row has its own Remove button. The
node's flash sector holds up to 30 networks — the form refuses row 31 with
that reason, never dropping a row silently:

On every boot (and whenever the link drops) the node scans, picks the
strongest KNOWN network and connects. While online it re-evaluates only
when the signal degrades below -75 dBm, and only hands over when another
known network beats the current one by at least 8 dB — a healthy link is
never churned. The tuning knobs (`WIFI_WEAK_RSSI_DBM`, `WIFI_ROAM_MARGIN_DB`,
...) sit at the top of `wifi.h`.

The node needs no port forwarding: it dials the relay itself. Behind an
extender/NAT router it behaves identically — if the link goes half-open,
the node detects the silence and reconnects on its own.

**GPIO choice:** the dashboard's Settings page defines the pin the big
On/Off toggle drives on every node. Default is GPIO 5 (D1 on NodeMCU/Wemos).
Avoid 0/1/2/3/15 (boot/serial strap pins) unless you know the wiring — the
settings page warns about them.

## 4. First install over the air (no cables)

The node carries no config file at all: its flash store is the only
credential and relay-configuration source, and the node knows WHERE it is
in its life. The
setup WiFi opens exactly when the first install is not complete — never
again on a mere outage.

| When the setup WiFi opens | |
| --- | --- |
| The node has NEVER joined a stored network (`provisioned` flag unset in flash): factory state, or credentials saved but never connected with | immediately after boot, and it serves until the form is saved — however long that takes |
| SETUP_MODE armed from the dashboard (the maintenance switch below) | immediately after boot, regardless of the provisioned flag; the armed flag clears itself when the setup form saves |
| A provisioned node (first install complete) that lost its WiFi | NEVER — it retries with the normal scan/connect flow; no captive portal reappears where anyone nearby could join it |

How the first install goes:

1. Power the node. Its setup WiFi appears as **`espnode-setup-`** plus the
   last 3 bytes of the chip id in lowercase hex, e.g. `espnode-setup-d4e5f6`
   — unique per board, so several bare nodes never collide.
2. Join it from a phone. The WPA2 password is **`espnode1234`** unless the
   build overrides it (`ESPNET_SETUP_AP_PASSWORD` as a -D build flag). The
   phone's captive-portal popup opens the setup page by itself; otherwise
   browse to **http://192.168.4.1/**.
3. Fill one row per network: name, password — or tick "open network" for a
   network without one (the "Add another network" button adds rows, each
   with its own Remove button). A blank password field on a row whose
   network is already stored keeps that password. The node always joins the
   STRONGEST of the saved networks, so list every floor/extender the node
   may sit on.
4. Fill the **Relay** section: the Pi's IP or hostname, the port (blank
   means 7777), and the relay's `nodeKey`. The node cannot do its job
   without them, so the save refuses to store half a configuration: the
   WiFi list and the relay settings are written together or not at all.
   The key is never shown back — a blank field keeps the stored one.
4. **Save and restart** — the page refuses anything incomplete without
   storing half of it (all-or-nothing), writes the list to the node's flash
   and restarts the node. The setup WiFi disappears; the node joins the
   strongest saved network and dials the relay as usual.

Details worth knowing:

- The setup page is served by the node itself (a captive DNS + a small HTTP
  server behind the WPA2 gate), with the same hardening the dashboard pages
  carry: a per-session CSRF token on the POST, strict request parsing with
  size caps, security headers, HTML-escaped output, and passwords are never
  echoed back into the page.
- The flash store is the ONLY credential source. The store also carries two flags: `provisioned` (set the
  moment the node's first WiFi connect succeeds, so a power cut right after
  cannot resurrect the setup WiFi) and `setup-armed` (cleared by the setup
  form's save). To start a node over from scratch, erase the flash —
  section 3, "Start a node over (erase the flash)" walks both ways (esptool
  or the IDE's "Erase Flash: All") and says what is cleared and what is
  kept. Reflashing the sketch alone does NOT clear the store, which is what
  keeps a working node working across updates.
- **Re-entering setup deliberately (SETUP_MODE):** the dashboard's
  Settings page has a "WiFi setup re-entry" maintenance switch. While it is
  ON, every node that (re)boots opens its setup WiFi instead of connecting;
  the setup form's save clears the armed flag on that node. Turn the switch
  on, power-cycle the ONE node to re-provision, save its WiFi, then turn the
  switch back off — it is fleet-wide by design (the relay's settings are one
  set), so treat it as a wrench, not a switch you leave on.
- The setup WiFi never opens while the node is connected and healthy, and
  never on a provisioned node in a WiFi outage. The setup WiFi's own password is public (it ships in
  the repository) — the gate keeps drive-by neighbours out, and the page
  can only write the WiFi list, nothing else.

## 5. OTA firmware updates

OTA replaces the sketch a node is ALREADY running, so it never serves as
the first install: flash a node's first image over a cable (section 3,
including the arduino-cli upload commands), and let this section take over
from the first release on.

1. Export/compile the sketch as a binary, e.g. with arduino-cli:
   ```bash
   arduino-cli compile --fqbn esp8266:esp8266:nodemcuv2 \
       --output-dir ./build arduino/ESP_node
   # -> ./build/ESP_node.ino.bin

   For a node on the other supported board: install its **core** (Boards
   Manager URL `https://espressif.github.io/arduino-esp32/package_esp32_index.json`),
   pick your exact board instead, and compile the SAME sketch folder:

   arduino-cli compile --fqbn esp32:esp32:esp32 \
       --output-dir ./build arduino/ESP_node
   # -> ./build/ESP_node.ino.bin (that board's image)

   GPIO legs are board-dependent: the ceilings and the excluded flash/strap
   pins live in `board.h`, and the guard there refuses a pushed pin the
   connected board cannot drive and keeps its current leg, so a pushed
   setting can never brick the node. The settings page repeats the safe
   range for the fleet.
   ```
   Bump `FW_VERSION` (the sketch's inline default) before compiling a new
   release.
2. Dashboard → Firmware → set the version, upload the `.bin`.
3. Nodes check the relay once a minute; when the served version differs
   from theirs they download, flash, log every step and reboot. Watch the
   Devices page's Firmware column (and the Logs page) as the fleet updates.

## 6. First bring-up walkthrough

1. Power a node → it appears on the Dashboard/Devices page within seconds
   (status: online, SSID, signal strength, firmware version, heap).
2. Click the big toggle → `gpio on` in the relay log, the node's log shows
   `gpio on on pin 5`, and the measured state arrives as an ack.
3. Settings → set the GPIO pin, the status cadence, the offline window, the
   node log level or names → saved and pushed to every online node
   immediately (offline nodes get it on their next reconnect). The settings
   page also shows the relay's read-only system summary (ports, key status,
   staged OTA image) — those are startup values from `config.js` on the Pi.
4. Logs → pick a node or the relay → the last 200 entries, newest first.

## 7. Troubleshooting

| Symptom | Where to look |
| --- | --- |
| Node never appears | relay log (`Logs → relay`): "rejected connection" = the node's stored fleet key (typed on its setup page) does not match the relay's `nodeKey` in `config.js`; "replacing stale/conflict" = two boards share one `DEVICE_ID`. |
| Node shown offline but LEDs on | it is connected but silent — check WiFi strength on the Devices page; the relay closes half-open links automatically. |
| Toggle shows "delivered to 0" | no node online right now — offline nodes catch up the moment they reconnect (desired state is persisted). |
| OTA never updates | Firmware page shows what the relay serves; node logs show each check. A refused upload means the file is not an ESP image (missing 0xE9 header). |
| Dashboard shows "relay unreachable" | the relay service is not running or `relay_url`/`relay_key` in `config.php` do not match the relay's `config.js`. |
| `setup.php` says "could not create a temporary file" | the web server cannot write the `espdash` folder — `sudo chown www-data:www-data /var/www/html/espdash` (`www-data` is the Debian/Raspberry Pi OS default; `ps aux | grep php-fpm` shows yours), then reload the page. The page detects this itself and shows the same fix before you fill the form. |
| The `espnode-setup-…` WiFi does not appear | the node is provisioned (it has completed a first successful WiFi connect, flag stored in flash) and is retrying — a provisioned node never auto-opens the setup WiFi. To re-enter setup deliberately: Settings page → "WiFi setup re-entry" ON, power-cycle the node, save its WiFi, switch OFF. The serial monitor shows the boot decision. |
| Node joined nothing after a save | wrong password or hidden network — arm SETUP_MODE on the dashboard, power-cycle the node, rejoin its setup WiFi and fix the row (a blank password field keeps the stored one); the save disarms the node again. To start over completely, erase the flash (section 3, "Start a node over (erase the flash)"). |
| `cp config.example.php config.php` says "Permission denied" | the `espdash` folder belongs to the web server user, so your own login cannot write there — run the manual copy through sudo as shown in section 2 (or use the setup page, which writes the file as the web server user). |

## 8. Running the tests

```bash
./run_tests.sh
```

- **Node unit tests** — relay decision logic (protocol framing, settings
  validation incl. the offline window and log level, device snapshots, log
  files, firmware rules).
- **Node e2e test** — starts the real relay on scratch ports, connects a
  simulated node over raw TCP (the same bytes the sketch sends) and walks
  the whole feature set: auth, status with SSID/RSSI, toggle delivery + ack,
  settings push (now incl. the log level), the `/api/config` system
  summary, logs, firmware upload/version/download, and the settings-driven
  offline window (a silent node flips to offline inside the saved window).
- **C++ logic tests** — the sketch's net/wifi/log/OTA modules compiled
  against a stub Arduino environment (g++): JSON readers, framing parity
  with the relay, message dispatch (incl. the pushed log level), repeat
  suppression, the log-level queue filter, strongest-network picking and
  the scan → connect → roam cycle. The radio/flash paths still need real
  hardware.
