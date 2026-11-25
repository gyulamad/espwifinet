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

// ── WiFi credentials are NOT here (task #135 v2) ──
// The node has NO compiled-in WiFi list: the networks picked on the node's
// own setup page are stored in the node's FLASH and are the only
// credentials it ever uses. A factory-new node (blank flash) opens its own
// "espnode-setup-xxxxxx" WiFi where a phone picks the network(s) — see
// INSTALL.md section 4. Nothing to edit here for the WiFi; config.h stays
// about the RELAY and the GPIO defaults.
//
// Optional: override the setup WiFi's own password (WPA2, 8..63 printable
// characters). The default is in the repository, so it is public knowledge:
// define ESPNET_SETUP_AP_PASSWORD here, before the sketch's includes, to
// replace it.
// #define ESPNET_SETUP_AP_PASSWORD "my-own-setup-key"

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
