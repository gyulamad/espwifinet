// config.h — the only file a new install must edit. Everything else (net.h,
// ota.h, remote_log.h) is generic and shared by all nodes of this network.
//
// The device id must be UNIQUE per board when set manually; leave DEVICE_ID
// empty ("") to derive it from the chip id, which is unique out of the box.
//
// The node connects OUT to the relay (RELAY_HOST:RELAY_PORT) — it never
// needs an inbound port on the node side, so it works behind WiFi extenders
// and NAT routers unchanged (that is the core architecture requirement, see
// task #33 and espcamsys2's push connection).

#pragma once

// ── WiFi ──
#define WIFI_SSID   "YOUR-WIFI-NAME"
#define WIFI_PASS   "YOUR-WIFI-PASSWORD"

// ── Relay (the Raspberry Pi) ──
#define RELAY_HOST  "192.168.1.10"   // relay's IP or hostname
#define RELAY_PORT  7777             // config.nodePort on the relay

// Must match nodeKey in the relay's config.js.
#define NODE_KEY    "CHANGE-ME-node-key"

// Empty = derive from the chip id ("esp-1A2B3C4D"). Set it only when the
// operator wants a stable human-readable name.
#define DEVICE_ID   ""

// Compiled-in defaults; the relay's settings page overrides gpioPin,
// gpioActiveHigh and the status interval at runtime.
#define DEFAULT_GPIO_PIN     5      // the leg the dashboard toggle drives (safe: D1 on NodeMCU/Wemos)
#define DEFAULT_ACTIVE_HIGH  true   // ON = HIGH; false for active-LOW relay boards
#define DEFAULT_STATUS_MS    10000  // status report cadence

// Firmware version for OTA — bump this on every release you upload
// (ota.h flashes the relay's image when its version differs from this).
#define FW_VERSION  "1.0.0"
