# Acronyms

Every acronym used in this project's docs (`README.md`, `INSTALL.md`), with
the meaning it carries here. Sorted alphabetically. Device, tool and project
names that look like acronyms are marked as names — they are listed because
the docs use them the same way and a reader may wonder.

| Acronym | Stands for | In this project |
| --- | --- | --- |
| API | Application Programming Interface | the relay's HTTP API that the PHP dashboard talks to (toggle calls, settings push, the `/api/config` system summary) |
| BE | Big-End(ian) | the wire protocol's `[4-byte BE length]` frame prefix — the length is written in network byte order |
| CLI | Command-Line Interface | `arduino-cli`, used to compile and export the node firmware binary |
| CSRF | Cross-Site Request Forgery | the dashboard login and forms carry CSRF tokens |
| CSP | Content Security Policy | the security header set on the dashboard pages |
| dBm | decibel-milliwatt (signal power unit) | WiFi signal strength on the Devices page; the node roams below -75 dBm and only when another network beats the current one by 8 dB |
| e2e | end-to-end | the Node test level in `run_tests.sh` that starts the real relay and drives it with a simulated node over raw TCP |
| ESP | Espressif (chip maker) — name | the ESP8266/ESP32 board family; "ESP Wifi Network" is this project's name |
| ESP-12E | Espressif module variant — name | the module on the "NodeMCU 1.0 (ESP-12E module)" board named in the flashing instructions |
| ESP8266 | Espressif Wi-Fi microcontroller — name | one of the two boards the sketch targets (with the ESP32/WROOM32) |
| FW | firmware | `FW_VERSION` in the sketch's `config.h`, bumped before compiling a new OTA release |
| GPIO | General-Purpose Input/Output | the pin the dashboard toggle drives on every node; default GPIO 5 (D1 on NodeMCU/Wemos boards) |
| HTML | HyperText Markup Language | the dashboard markup, escaped through one helper |
| HTTP | HyperText Transfer Protocol | the relay's API transport (GET requests), OTA firmware serving and dashboard↔relay calls |
| IDE | Integrated Development Environment | the Arduino IDE used to flash the node |
| IO | Input/Output | kept in thin wrappers so every module's decision logic stays pure and testable |
| IP | Internet Protocol (address) | the Pi's address, set as `RELAY_HOST` in the sketch and in the dashboard's relay URL |
| JSON | JavaScript Object Notation | the framed node↔relay protocol, the JSON-lines log files and the `settings.json`/`state.json` data files |
| LAN | Local Area Network | the home network the dashboard is meant to be used on |
| LED | Light-Emitting Diode | troubleshooting sign: a node shown offline but with LEDs on is connected yet silent |
| md5 | Message-Digest algorithm 5 | the checksum stored alongside the served OTA image |
| NAT | Network Address Translation | NAT routers/extenders the nodes work behind unchanged, because every connection is outbound |
| npm | Node.js package manager (officially not an acronym) — name | installs the relay's dependencies (`npm install --omit=dev`) |
| NodeMCU | "Node MicroController Unit" (board name) — name | the ESP8266 board family the flashing instructions name |
| OTA | Over-The-Air | the firmware self-update: nodes poll the relay, download, flash and reboot |
| PHP | "PHP: Hypertext Preprocessor" (recursive) | the dashboard frontend; PHP ≥ 8 with the curl extension |
| RSSI | Received Signal Strength Indicator | the per-node signal strength carried in every status report |
| SSID | Service Set Identifier | the WiFi network name every node reports and the multi-wifi list is built from |
| SSH | Secure Shell | the tunnel alternative for exposing the dashboard beyond the LAN |
| TCP | Transmission Control Protocol | the node→relay raw socket connection (port 7777) and the e2e test transport |
| TLS | Transport Layer Security | the encryption to put in front of the dashboard before exposing it beyond the home LAN |
| URL | Uniform Resource Locator | the relay URL setting, the Arduino Boards Manager URL and the firmware endpoints |
| WiFi | wireless networking (Wi-Fi brand, not a strict acronym) — name | the subject of the multi-network roaming, extenders, SSID/RSSI reporting |
| X-API-Key | HTTP header carrying the API key | the shared-secret header the PHP dashboard sends to the relay |
