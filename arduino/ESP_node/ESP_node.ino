// ESP_node.ino — one node of the ESP Wifi Network (task #33 base
// project, #36 setup & settings, #135 first install). Role of this sketch:
//   - join WiFi through the multi-network manager (wifi.h): always
//     (re)connects to the STRONGEST of the known networks — the list comes
//     from the node's flash store (wifi_store.h, written by the setup page);
//     an empty store means a factory node that opens its setup WiFi (there
//     is no compiled fallback — task #140),
//   - a FACTORY-NEW node (no credentials anywhere) opens its own setup WiFi
//     (setup_logic.h / setup_ap.h): join "espnode-setup-xxxxxx" from a
//     phone, pick the network(s) and password(s) on the page, save — the
//     node stores them in flash and restarts into the normal flow,
//   - a node that cannot join ANY known network for a while reopens the
//     setup WiFi in bounded windows, so a wrong password can always be
//     fixed from the phone that set it (no cable, no reflash),
//   - keep one OUTBOUND connection to the relay (works behind WiFi
//     extenders / NAT routers — see net.h),
//   - report status (SSID, signal strength, heap, uptime, gpio) so the
//     dashboard shows every board's connection state,
//   - drive the settings-defined GPIO leg from the dashboard's single
//     On/Off toggle (commands arrive on the same connection),
//   - log to the relay (remote_log.h) and keep itself updated over OTA
//     (ota.h).
//
// Files: board.h, wifi_store.h, wifi_store_hw.h, setup_logic.h,
// setup_ap.h, wifi.h, net.h, ota.h, remote_log.h. There is NO config file
// (task #140): the WiFi list AND the relay address + fleet key live in the
// node's flash, written by its own setup page — a flashed binary plus one
// phone visit is a complete install, and the end user never touches source.
// Board: any supported ESP board (#131) — one sketch, board.h carries the
// few board-specific bits (includes, chip id, watchdog, GPIO ceiling).
// Settings from the relay override the compiled-in GPIO pin, active level,
// status interval and log level at runtime, so the sketch usually needs no
// reflash.

#include "board.h"

#include "net.h"
#include "ota.h"
#include "remote_log.h"
#include "setup_ap.h"
#include "setup_logic.h"
#include "wifi.h"
#include "wifi_store.h"
#include "wifi_store_hw.h"

// ── build-time defaults — there is NO config.h (task #140) ──
// Everything a node NEEDS comes over the air: the WiFi list, the relay
// address and the fleet key via the setup page; gpioPin/active level/
// status interval/log level via the relay's settings push. These #ifndef
// lines exist so a builder CAN override a default with -D flags without
// touching the source — the end user never does.
#ifndef ESPNET_SETUP_AP_PASSWORD
#define ESPNET_SETUP_AP_PASSWORD "espnode1234"  // the setup WiFi's WPA2 password (public by design, per-build overridable)
#endif
#ifndef FW_VERSION
#define FW_VERSION "2.0.0"  // bumped on every shipped release; OTA flashes the relay's image when it differs
#endif
#ifndef DEFAULT_GPIO_PIN
#define DEFAULT_GPIO_PIN 5
#endif
#ifndef DEFAULT_ACTIVE_HIGH
#define DEFAULT_ACTIVE_HIGH true
#endif
#ifndef DEFAULT_STATUS_MS
#define DEFAULT_STATUS_MS 10000
#endif
#ifndef DEFAULT_MIN_LOG_LEVEL
#define DEFAULT_MIN_LOG_LEVEL 0
#endif

// ── credentials: the flash store is the ONLY source (task #135 v2) ──
// config.h carries no WiFi list anymore: the networks picked on the setup
// page live in the node's flash (wifi_store.h), together with the two
// life-cycle flags. The list is copied into RAM buffers that outlive
// setup(), so the WifiNetwork views (const char*) stay valid for the
// sketch's lifetime.
static espnet::EepromRegionBackend storeBackend(0, espnet::WIFI_STORE_REGION_MAX);
static espnet::EepromRegionBackend relayBackend(espnet::RELAY_CONFIG_BASE, espnet::RELAY_CONFIG_REGION_MAX);
static espnet::WifiStore wifiStore;
static espnet::RelayConfigStore relayConfigStore;
static espnet::RelayConfig relayCfg;       // the flash-held relay host/port/key (#140)
static bool relayConfigured = false;
static espnet::WifiStoreNet storedNets[espnet::WIFI_STORE_MAX_NETS];
static int storedCount = 0;
static std::string effectiveSsid[espnet::WIFI_STORE_MAX_NETS];
static std::string effectivePass[espnet::WIFI_STORE_MAX_NETS];
static espnet::WifiNetwork effectiveNets[espnet::WIFI_STORE_MAX_NETS];
static int effectiveCount = 0;
static bool provisionedDone = false;   // the store's provisioned flag, mirrored

// ── the setup WiFi (task #135 v2) ──
// bootPortalDecision (setup_logic.h): it opens exactly when the first
// install is not complete (factory, or saved-but-never-connected) or the
// operator armed SETUP_MODE — and then serves FOREVER until the form saves
// (the save also disarms). A provisioned node on a WiFi outage just retries:
// no captive portal reappears where anyone nearby could join it.
static espnet::SetupPortal portal;
static bool portalActive = false;

// ── identity: "esp-XXXXXXXX" derived from the chip id (unique out of the
// box) — nothing to edit anywhere (#140). ──
static String deviceAuto;
static const char* DEVICE = nullptr; // filled in setup()

static const char* FW = FW_VERSION;

// ── runtime state ──
static espnet::RemoteLog nodeLog;
static espnet::RelayConnection relay;
static espnet::OtaUpdater ota;
static espnet::WifiMulti wifiMulti;

static long gpioPin = DEFAULT_GPIO_PIN;      // overridden by relay settings
static bool gpioActiveHigh = DEFAULT_ACTIVE_HIGH;
static bool gpioStateOn = false;             // desired state of the switch
static long statusIntervalMs = DEFAULT_STATUS_MS;
static long minLogLevel = DEFAULT_MIN_LOG_LEVEL; // overridden by relay settings

static uint32_t lastStatusMs = 0;
static uint32_t nextConnectAttemptMs = 0;
static uint32_t connectBackoffMs = espnet::CONNECT_BACKOFF_MIN_MS;

// ── GPIO ──
static void applyGpio() {
  pinMode((uint8_t)gpioPin, OUTPUT);
  digitalWrite((uint8_t)gpioPin, gpioStateOn ? (gpioActiveHigh ? HIGH : LOW)
                                             : (gpioActiveHigh ? LOW : HIGH));
}

static const char* gpioStateText() {
  return gpioStateOn ? "on" : "off";
}

// ── net hooks (see NetHooks in net.h) ──
static void onSettings(long pin, bool activeHigh, long statusMs, long minLevel) {
  if (espnet::gpioPinAllowed(pin) && pin != gpioPin) {
    nodeLog.logf(espnet::LOG_INFO, "settings: gpio pin %ld -> %ld", gpioPin, pin);
    gpioPin = pin;
  }
  if (activeHigh != gpioActiveHigh) {
    nodeLog.logf(espnet::LOG_INFO, "settings: active level -> %s", activeHigh ? "high" : "low");
    gpioActiveHigh = activeHigh;
  }
  if (statusMs >= 2000 && statusMs <= 600000 && statusMs != statusIntervalMs) {
    nodeLog.logf(espnet::LOG_INFO, "settings: status interval %ld -> %ld ms", statusIntervalMs, statusMs);
    statusIntervalMs = statusMs;
  }
  if (minLevel >= 0 && minLevel <= 2 && minLevel != minLogLevel) {
    nodeLog.logf(espnet::LOG_INFO, "settings: min log level %ld -> %ld", minLogLevel, minLevel);
    minLogLevel = minLevel;
    nodeLog.setMinLevel((espnet::LogLevel)minLevel);
  }
  applyGpio(); // the pin (or level) may have changed
}

static void onSetGpio(const char* state, long pin, bool activeHigh) {
  gpioStateOn = (strcmp(state, "on") == 0);
  if (espnet::gpioPinAllowed(pin)) gpioPin = pin;
  if (espnet::gpioPinAllowed(pin)) gpioActiveHigh = activeHigh;
  applyGpio();
  nodeLog.logf(espnet::LOG_INFO, "gpio %s on pin %ld", gpioStateText(), gpioPin);
  // ack so the dashboard shows the achieved state, not just the desired one
  relay.sendGpioAck(gpioStateText(), gpioPin);
}

static void onSetupMode(bool armed) {
  if (!armed || wifiStore.setupArmed()) return; // only the rising edge arms
  if (wifiStore.armSetup(true)) {
    nodeLog.logf(espnet::LOG_WARN,
                 "setup armed from the dashboard - restarting into the setup WiFi '%s'",
                 portal.ssid().c_str());
    ESP.restart(); // the boot decision (armed) opens the setup WiFi
  } else {
    nodeLog.logf(espnet::LOG_ERROR, "setup: could not write the armed flag to flash");
  }
}

static espnet::NetHooks makeHooks() {
  espnet::NetHooks hooks;
  hooks.onSettings = onSettings;
  hooks.onSetGpio = onSetGpio;
  hooks.onSetupMode = onSetupMode;
  return hooks;
}

// ── relay connection lifecycle ──
static void ensureRelay() {
  if (relay.connected()) {
    // Half-open link detection: a NAT box can drop the mapping without RST.
    if (relay.silentMs() > espnet::RELAY_SILENT_LIMIT_MS) {
      nodeLog.logf(espnet::LOG_WARN, "relay silent %lu ms — reconnecting", (unsigned long)relay.silentMs());
      relay.reset();
    }
    return;
  }
  const uint32_t now = millis();
  if (now < nextConnectAttemptMs) return;
  nextConnectAttemptMs = now + connectBackoffMs;
  if (relay.connectAndSayHello(FW, ESPNET_CHIP_NAME)) {
    nodeLog.logf(espnet::LOG_INFO, "relay: connected");
    connectBackoffMs = espnet::CONNECT_BACKOFF_MIN_MS;
    lastStatusMs = 0; // send a status right away
  } else {
    connectBackoffMs = min(connectBackoffMs * 2, espnet::CONNECT_BACKOFF_MAX_MS);
    nodeLog.logf(espnet::LOG_INFO, "relay: dial failed, backoff %lu ms", (unsigned long)connectBackoffMs);
  }
}

// ── periodic status ──
static void maybeSendStatus() {
  if (!relay.connected()) return;
  const uint32_t now = millis();
  if (now - lastStatusMs < (uint32_t)statusIntervalMs) return;
  lastStatusMs = now;
  relay.sendStatus(gpioPin, gpioStateText());
}

// ── queued remote log delivery (bounded ring; retried on reconnect) ──
static void flushLogs() {
  while (nodeLog.pending() && relay.connected()) {
    espnet::LogEntry entry;
    if (!nodeLog.pop(entry)) break;
    relay.sendLogLine(entry);
  }
}

void setup() {
  Serial.begin(115200);
  delay(200);
  nodeLog.begin(&Serial);

  // resolve the device id once: chip-id derived, unique out of the box
  char buf[24];
  snprintf(buf, sizeof(buf), "esp-%08X", (unsigned)espnet::chipId());
  deviceAuto = String(buf);
  DEVICE = deviceAuto.c_str();

  applyGpio();
  nodeLog.setMinLevel((espnet::LogLevel)minLogLevel);
  nodeLog.logf(espnet::LOG_INFO, "boot: %s fw %s gpio pin %ld (%s)", DEVICE, FW, gpioPin,
               gpioActiveHigh ? "active-high" : "active-low");

  // ── credentials + relay configuration: flash is the ONLY source ──
  // (#135 v2 the WiFi list, #140 the relay host/port/key)
  storeBackend.begin();
  wifiStore.begin(&storeBackend);
  relayBackend.begin();
  relayConfigStore.begin(&relayBackend);
  relayConfigured = relayConfigStore.load(relayCfg);
  storedCount = wifiStore.load(storedNets, espnet::WIFI_STORE_MAX_NETS);
  provisionedDone = wifiStore.provisioned();
  if (espnet::effectiveNetworksCount(storedNets, storedCount) > 0) {
    for (int i = 0; i < storedCount; ++i) {
      effectiveSsid[i] = storedNets[i].ssid;
      effectivePass[i] = storedNets[i].pass;
      effectiveNets[i].ssid = effectiveSsid[i].c_str();
      effectiveNets[i].pass = effectivePass[i].c_str();
    }
    effectiveCount = storedCount;
    nodeLog.logf(espnet::LOG_INFO, "credentials: %d from flash (setup page)", effectiveCount);
  } else {
    storedCount = 0; // damaged rows are never handed to the picker
    nodeLog.logf(espnet::LOG_INFO, "credentials: flash store empty or damaged (%d usable)", storedCount);
  }

  // The boot decision (setup_logic.h, desktop-tested): the setup WiFi opens
  // exactly when the first install is not complete or SETUP_MODE is armed —
  // and then serves FOREVER until the form saves. A provisioned node NEVER
  // auto-opens it (a WiFi outage just retries).
  const bool portalWanted =
      espnet::bootPortalDecision(wifiStore.setupArmed(), provisionedDone,
                                 effectiveCount, relayConfigured) == espnet::PortalDecision::Portal;
  if (portalWanted) {
    portal.begin(wifiStore, relayConfigStore);
    nodeLog.logf(espnet::LOG_WARN,
                 "first install not complete (provisioned=%d armed=%d relay=%d) - opening the setup WiFi; "
                 "join '%s' (password in INSTALL.md) and pick the network(s) + relay there",
                 provisionedDone ? 1 : 0, wifiStore.setupArmed() ? 1 : 0, relayConfigured ? 1 : 0,
                 portal.ssid().c_str());
    portal.start(0); // serve forever
    portalActive = true;
  }

  wifiMulti.begin(effectiveNets, effectiveCount, &nodeLog);
  relay.begin(relayCfg.host.c_str(), relayCfg.port, DEVICE, relayCfg.key.c_str(), &nodeLog, makeHooks());
  ota.begin(relayCfg.host.c_str(), relayCfg.port, &nodeLog);
}

void loop() {
  espnet::feedWdt();

  if (portalActive) {
    const espnet::SetupStep step = portal.tick(millis());
    if (step == espnet::SetupStep::Restart) {
      ESP.restart(); // the saved credentials take over (session stopped the AP)
      return;
    }
    if (step == espnet::SetupStep::Handback) {
      // Defensive: the sketch always serves forever (serveCapMs 0), so a
      // handback should not happen. Resume the normal flow anyway.
      portalActive = false;
      nodeLog.logf(espnet::LOG_INFO, "setup: closed - resuming the normal WiFi flow");
      wifiMulti.begin(effectiveNets, effectiveCount, &nodeLog);
    }
    flushLogs();
    return;
  }

  wifiMulti.ensure(); // scan → pick the strongest known network → connect

  if (wifiMulti.connected()) {
    if (!provisionedDone) {
      // The FIRST successful WiFi connect: the first install is complete
      // (v2). Persisted at once, so a power cut cannot resurrect the setup
      // WiFi on a provisioned node.
      provisionedDone = wifiStore.markProvisioned();
      nodeLog.logf(espnet::LOG_INFO, "wifi: first connect - node is provisioned (%s)",
                   provisionedDone ? "stored" : "store write FAILED");
    }
    ensureRelay();
    if (relay.connected()) {
      if (!relay.pumpMessages()) {
        nodeLog.logf(espnet::LOG_ERROR, "relay: protocol error — reconnecting");
        relay.reset();
      }
      maybeSendStatus();
    }
    ota.tick(FW);
  }
  // No fallback portal: a provisioned node in an outage just retries
  // (bootPortalDecision). Re-entry is deliberate — arm SETUP_MODE from the
  // dashboard and power-cycle the node.

  flushLogs();
}
