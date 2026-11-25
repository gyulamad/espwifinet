// config.h — the only file a new install must edit. Everything else (net.h,
// ota.h, remote_log.h) is generic and shared by all nodes of this network.
//
// The device id must be UNIQUE per board when set manually; leave DEVICE_ID
// empty ("") to derive it from the chip id, which is unique out of the box.
//
// The node connects OUT to the relay (RELAY_HOST:RELAY_PORT) — it never
// needs an inbound port on the node side, so it works behind WiFi extenders
// and NAT routers unchanged (that is the core architecture requirement, see
// task #33).

#pragma once

// ── WiFi: the node knows a LIST of networks and (re)connects to the
// strongest one it can see (task #36, the WiFiMulti pattern). List every network a node may sit on — different floors,
// rooms or extenders. Leave the SSID empty ("") for unused slots.
//
// This list is the FALLBACK: the networks picked on the node's own setup
// page (task #135 — a factory-new node opens the "espnode-setup-xxxxxx"
// WiFi) are stored in the node's flash and override it. Compile-in
// credentials only when you want an install to work with NO setup-page
// visit; leave the slots empty and the setup page walks the user through
// (that is what makes a first install cable-free).
//
// Optional: override the setup WiFi's own password (WPA2, 8..63 printable
// characters). The default is in the repository, so it is public knowledge:
// define ESPNET_SETUP_AP_PASSWORD here, before the sketch's includes, to
// replace it.
// #define ESPNET_SETUP_AP_PASSWORD "my-own-setup-key"
#define WIFI_NETWORK_1_SSID  "YOUR-WIFI-NAME"
#define WIFI_NETWORK_1_PASS  "YOUR-WIFI-PASSWORD"
#define WIFI_NETWORK_2_SSID  ""
#define WIFI_NETWORK_2_PASS  ""
#define WIFI_NETWORK_3_SSID  ""
#define WIFI_NETWORK_3_PASS  ""

// ── Relay (the Raspberry Pi) ──
#define RELAY_HOST  "192.168.1.10"   // relay's IP or hostname
#define RELAY_PORT  7777             // config.nodePort on the relay

// Must match nodeKey in the relay's config.js.
#define NODE_KEY    "CHANGE-ME-node-key"

// Empty = derive from the chip id ("esp-1A2B3C4D"). Set it only when the
// operator wants a stable human-readable name.
#define DEVICE_ID   ""

// Compiled-in defaults; the relay's settings page overrides gpioPin,
// gpioActiveHigh, the status interval and the lowest log level the node
// queues at runtime (see net.h's "settings" message).
#define DEFAULT_GPIO_PIN     5      // the leg the dashboard toggle drives (safe: D1 on NodeMCU/Wemos)
#define DEFAULT_ACTIVE_HIGH  true   // ON = HIGH; false for active-LOW relay boards
#define DEFAULT_STATUS_MS    10000  // status report cadence
#define DEFAULT_MIN_LOG_LEVEL 0     // 0 = info (queue everything), 1 = warn, 2 = error

// Firmware version for OTA — bump this on every release you upload
// (ota.h flashes the relay's image when its version differs from this).
#define FW_VERSION  "1.0.0"
