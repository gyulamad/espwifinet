# ESP Wifi Network — base project

A small, extensible base for switching GPIOs on ESP8266 boards from a
dashboard, following the architecture of `gyulamad/espcamsys2`:

```
 browser ──► PHP dashboard (Apache/PHP on the Pi)
                │  HTTP + X-API-Key (shared secret)
                ▼
            Node.js relay (the Raspberry Pi)          firmware .bin
                ▲                        ▲                │
   outbound TCP │                        │ HTTP GET       │ upload via dashboard
   (auth line + │                        │ /firmware/*    ▼
   framed JSON) │                 ESP8266 nodes ◄── OTA self-update
                └──── ESP8266 nodes ─────┘
        (behind WiFi extenders / NAT routers: no inbound port needed)
```

Each ESP8266 node keeps **one outbound TCP connection** to the relay. That is
the key architectural decision (taken from espcamsys2's push connection):
the node dials the relay, so nodes work unchanged behind WiFi extenders and
NAT routers, and the same established connection carries the relay's
commands back down (GPIO toggle, settings, OTA trigger).

Directory layout:

| path | what it is |
| --- | --- |
| `nodejs/esp-relay/` | the relay: raw TCP hub for nodes + HTTP API for the dashboard |
| `php/espdash/` | the PHP frontend: login, dashboard (one toggle), devices, logs, settings, firmware |
| `arduino/ESP8266_Node/` | the node sketch: connection, status reporting, remote logging, OTA, GPIO |
| `tests/` | unit + e2e (Node) and logic (C++) tests; `./run_tests.sh` runs everything |

---

## 1. Install the relay (Raspberry Pi)

Requires Node.js ≥ 18 (`node --version`) on the Pi.

```bash
# on the Pi
sudo mkdir -p /opt/espwifinet
sudo cp -r nodejs/esp-relay /opt/espwifinet/
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
User=pi

[Install]
WantedBy=multi-user.target
```

```bash
sudo systemctl daemon-reload
sudo systemctl enable --now esp-relay
```

Data written by the relay (all inside the relay folder, add to your backups):

- `data/settings.json` — GPIO pin, active level, status interval, node names
- `data/state.json` — the dashboard toggle's desired state
- `logs/<device>.log` — one JSON-lines file per node + `relay.log`
- `firmware/` — the currently served OTA image + version + md5

## 2. Install the PHP dashboard

Requires PHP ≥ 8 with the `curl` extension (`php -m | grep curl`).

```bash
sudo cp -r php/espdash /var/www/html/espdash
cd /var/www/html/espdash

# 1) create your config
cp config.example.php config.php
nano config.php
#   relay_url : http://127.0.0.1:8080 (the relay you installed above)
#   relay_key : the SAME key as relayKey in the relay's config.js
#   users     : generate a password hash and paste it:
php -r "echo password_hash('YourSecretPassword', PASSWORD_DEFAULT);"
```

Open `http://<pi-ip>/espdash/`, sign in, and you are done. The dashboard is
used over plain HTTP on a home LAN; put it behind TLS (or an SSH tunnel)
before exposing it anywhere beyond that.

## 3. Flash an ESP8266 node

In the Arduino IDE (or arduino-cli):

1. Install the **ESP8266 core** (Boards Manager URL:
   `https://arduino.esp8266.com/stable/package_esp8266com_index.json`).
2. Open `arduino/ESP8266_Node/ESP8266_Node.ino`.
3. Edit `config.h`: WiFi credentials, `RELAY_HOST` (the Pi's IP),
   `RELAY_PORT` (7777), `NODE_KEY` (the same key as `nodeKey` in the
   relay's config.js). Leave `DEVICE_ID` empty to auto-derive a unique id
   from the chip.
4. Board "NodeMCU 1.0 (ESP-12E module)" (or your board), Upload.
5. Open the Serial Monitor (115200) — the node logs its boot, WiFi join and
   relay connection there, and every log line also appears on the
   dashboard's Logs page.

The node needs no port forwarding: it dials the relay itself. Behind an
extender/NAT router it behaves identically — if the link goes half-open,
the node detects the silence and reconnects on its own.

**GPIO choice:** the dashboard's Settings page defines the pin the big
On/Off toggle drives on every node. Default is GPIO 5 (D1 on NodeMCU/Wemos).
Avoid 0/1/2/3/15 (boot/serial strap pins) unless you know the wiring — the
settings page warns about them.

## 4. OTA firmware updates

1. Export/compile the sketch as a binary, e.g. with arduino-cli:
   ```bash
   arduino-cli compile --fqbn esp8266:esp8266:nodemcuv2 \
       --output-dir ./build arduino/ESP8266_Node
   # -> ./build/ESP8266_Node.ino.bin
   ```
   Bump `FW_VERSION` in `config.h` before compiling a new release.
2. Dashboard → Firmware → set the version, upload the `.bin`.
3. Nodes check the relay once a minute; when the served version differs
   from theirs they download, flash, log every step and reboot. Watch the
   Devices page's Firmware column (and the Logs page) as the fleet updates.

## 5. First bring-up walkthrough

1. Power a node → it appears on the Dashboard/Devices page within seconds
   (status: online, SSID, signal strength, firmware version, heap).
2. Click the big toggle → `gpio on` in the relay log, the node's log shows
   `gpio on on pin 5`, and the measured state arrives as an ack.
3. Settings → set the GPIO pin / names → saved and pushed to every online
   node immediately (offline nodes get it on their next reconnect).
4. Logs → pick a node or the relay → the last 200 entries, newest first.

## 6. Troubleshooting

| Symptom | Where to look |
| --- | --- |
| Node never appears | relay log (`Logs → relay`): "rejected connection" = wrong `NODE_KEY` in the sketch; "replacing stale/conflict" = two boards share one `DEVICE_ID`. |
| Node shown offline but LEDs on | it is connected but silent — check WiFi strength on the Devices page; the relay closes half-open links automatically. |
| Toggle shows "delivered to 0" | no node online right now — offline nodes catch up the moment they reconnect (desired state is persisted). |
| OTA never updates | Firmware page shows what the relay serves; node logs show each check. A refused upload means the file is not an ESP8266 image (missing 0xE9 header). |
| Dashboard shows "relay unreachable" | the relay service is not running or `relay_url`/`relay_key` in `config.php` do not match the relay's `config.js`. |

## 7. Running the tests

```bash
./run_tests.sh
```

- **Node unit tests** — relay decision logic (protocol framing, settings
  validation, device snapshots, log files, firmware rules).
- **Node e2e test** — starts the real relay on scratch ports, connects a
  simulated node over raw TCP (the same bytes the sketch sends) and walks
  the whole feature set: auth, status with SSID/RSSI, toggle delivery + ack,
  settings push, logs, firmware upload/version/download, offline marking.
- **C++ logic tests** — the sketch's net/log/OTA modules compiled against a
  stub Arduino environment (g++): JSON readers, framing parity with the
  relay, message dispatch, repeat suppression, bounded log queue. The
  radio/flash paths still need real hardware, as in espcamsys2.
