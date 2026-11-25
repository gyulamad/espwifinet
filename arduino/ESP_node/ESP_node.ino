// ESP_node.ino — one node of the ESP Wifi Network (task #33 base
// project, #36 setup & settings, #135 first install). Role of this sketch:
//   - join WiFi through the multi-network manager (wifi.h): always
//     (re)connects to the STRONGEST of the known networks — the list comes
//     from the node's flash store (wifi_store.h, written by the setup page)
//     or, when that is empty, from config.h,
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
// Files: config.h (edit per install), board.h, wifi_store.h, wifi_store_hw.h,
// setup_logic.h, setup_ap.h, wifi.h, net.h, ota.h, remote_log.h.
// Board: any ESP8266 (NodeMCU 1.0 / Wemos D1 mini, the tested target) or
// ESP32 / WROOM32 (#131) — one sketch for both, board.h carries the few
// board-specific bits (includes, chip id, watchdog, GPIO ceiling).
// Settings from the relay override the compiled-in GPIO pin, active level,
// status interval and log level at runtime, so the sketch usually needs no
// reflash.

#include "board.h"

#include "config.h"
#include "net.h"
#include "ota.h"
#include "remote_log.h"
#include "setup_ap.h"
#include "setup_logic.h"
#include "wifi.h"
#include "wifi_store.h"
#include "wifi_store_hw.h"

// Compiled-in defaults; the relay's settings page overrides gpioPin,
// gpioActiveHigh, the status interval and the log level at runtime.
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

// The known networks (config.h) — the FALLBACK list: used only when the
// flash store (wifi_store.h) holds nothing usable. Unused slots keep an
// empty ssid and are skipped by the strongest-network picker (wifi.h).
static espnet::WifiNetwork NETWORKS[] = {
    {WIFI_NETWORK_1_SSID, WIFI_NETWORK_1_PASS},
    {WIFI_NETWORK_2_SSID, WIFI_NETWORK_2_PASS},
    {WIFI_NETWORK_3_SSID, WIFI_NETWORK_3_PASS},
};

// ── credentials: the flash store first, config.h's list second (#135) ──
// The setup page writes the store; a valid, non-empty store overrides the
// compiled-in list, an empty or damaged one falls back to it. The effective
// list lives in RAM buffers that outlive setup(), so the WifiNetwork views
// (const char*) stay valid for the sketch's lifetime.
static espnet::EepromWifiStoreBackend storeBackend;
static espnet::WifiStore wifiStore;
static espnet::WifiStoreNet storedNets[espnet::WIFI_STORE_MAX_NETS];
static int storedCount = 0;
static std::string effectiveSsid[espnet::WIFI_STORE_MAX_NETS];
static std::string effectivePass[espnet::WIFI_STORE_MAX_NETS];
static espnet::WifiNetwork effectiveNets[espnet::WIFI_STORE_MAX_NETS];
static int effectiveCount = 0;

// ── the setup WiFi (task #135) ──
// A node with NOTHING to join with opens it at once and serves until the
// form saves (serveCapMs 0 = forever). A node WITH credentials that has not
// joined anything for WIFI_FALLBACK_SETUP_MS opens it in bounded windows of
// SETUP_SERVE_MAX_MS, so a temporary outage cannot take a working node
// hostage: after each window the normal scan/connect flow resumes, and any
// successful connection resets the timer.
#ifndef WIFI_FALLBACK_SETUP_MS
#define WIFI_FALLBACK_SETUP_MS 300000  // disconnected this long -> reopen setup (5 min)
#endif
#ifndef SETUP_SERVE_MAX_MS
#define SETUP_SERVE_MAX_MS 600000      // one fallback window (10 min)
#endif
static espnet::SetupPortal portal;
static bool portalActive = false;
static uint32_t lastOnlineMs = 0;

// ── identity: the operator-set DEVICE_ID from config.h, or "esp-XXXXXXXX"
// derived from the chip id (unique out of the box). Chosen at runtime. ──
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

static espnet::NetHooks makeHooks() {
  espnet::NetHooks hooks;
  hooks.onSettings = onSettings;
  hooks.onSetGpio = onSetGpio;
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

  // resolve the device id once (config.h's DEVICE_ID, else chip-id derived)
  if (DEVICE_ID[0] != '\0') {
    DEVICE = DEVICE_ID;
  } else {
    char buf[24];
    snprintf(buf, sizeof(buf), "esp-%08X", (unsigned)espnet::chipId());
    deviceAuto = String(buf);
    DEVICE = deviceAuto.c_str();
  }

  applyGpio();
  nodeLog.setMinLevel((espnet::LogLevel)minLogLevel);
  nodeLog.logf(espnet::LOG_INFO, "boot: %s fw %s gpio pin %ld (%s)", DEVICE, FW, gpioPin,
               gpioActiveHigh ? "active-high" : "active-low");

  // ── credentials: flash store first, config.h's compiled list second ──
  storeBackend.begin();
  wifiStore.begin(&storeBackend);
  storedCount = wifiStore.load(storedNets, espnet::WIFI_STORE_MAX_NETS);
  if (espnet::effectiveNetworksCount(storedNets, storedCount) > 0) {
    for (int i = 0; i < storedCount; ++i) {
      effectiveSsid[i] = storedNets[i].ssid;
      effectivePass[i] = storedNets[i].pass;
      effectiveNets[i].ssid = effectiveSsid[i].c_str();
      effectiveNets[i].pass = effectivePass[i].c_str();
    }
    effectiveCount = storedCount;
    nodeLog.logf(espnet::LOG_INFO, "credentials: %d from flash (setup page), config.h list unused", effectiveCount);
  } else {
    for (unsigned i = 0; i < sizeof(NETWORKS) / sizeof(NETWORKS[0]) && effectiveCount < espnet::WIFI_STORE_MAX_NETS; ++i) {
      if (NETWORKS[i].ssid == nullptr || NETWORKS[i].ssid[0] == '\0') continue;
      effectiveSsid[effectiveCount] = NETWORKS[i].ssid;
      effectivePass[effectiveCount] = NETWORKS[i].pass;
      effectiveNets[effectiveCount].ssid = effectiveSsid[effectiveCount].c_str();
      effectiveNets[effectiveCount].pass = effectivePass[effectiveCount].c_str();
      ++effectiveCount;
    }
    nodeLog.logf(espnet::LOG_INFO, "credentials: %d compiled in (config.h), flash store %s",
                 effectiveCount, storedCount > 0 ? "damaged" : "empty");
  }

  if (effectiveCount == 0) {
    // Factory state: nothing to join with anywhere — open the setup WiFi
    // and serve the form until it saves (forever, the session retries on
    // its own if the AP start fails).
    portal.begin(wifiStore);
    nodeLog.logf(espnet::LOG_WARN,
                 "no credentials anywhere — opening the setup WiFi; join '%s' "
                 "(password in INSTALL.md) and pick the network(s) there",
                 portal.ssid().c_str());
    portal.start(0); // serve forever
    portalActive = true;
  }

  wifiMulti.begin(effectiveNets, effectiveCount, &nodeLog);
  relay.begin(RELAY_HOST, RELAY_PORT, DEVICE, NODE_KEY, &nodeLog, makeHooks());
  ota.begin(RELAY_HOST, RELAY_PORT, &nodeLog);
  lastOnlineMs = millis();
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
      // A bounded fallback window closed without a save: resume the normal
      // scan/connect flow; the timer reopens the setup WiFi if nothing
      // joins for another WIFI_FALLBACK_SETUP_MS.
      portalActive = false;
      lastOnlineMs = millis();
      nodeLog.logf(espnet::LOG_INFO, "setup: window closed - resuming the normal WiFi flow");
      wifiMulti.begin(effectiveNets, effectiveCount, &nodeLog);
    }
    flushLogs();
    return;
  }

  wifiMulti.ensure(); // scan → pick the strongest known network → connect

  if (wifiMulti.connected()) {
    lastOnlineMs = millis();
    ensureRelay();
    if (relay.connected()) {
      if (!relay.pumpMessages()) {
        nodeLog.logf(espnet::LOG_ERROR, "relay: protocol error — reconnecting");
        relay.reset();
      }
      maybeSendStatus();
    }
    ota.tick(FW);
  } else if (effectiveCount > 0 && millis() - lastOnlineMs > (uint32_t)WIFI_FALLBACK_SETUP_MS) {
    // Nothing joined for a full window: the credentials are probably wrong
    // or the network is gone — open the setup WiFi in a bounded window so
    // the phone that set the node up can fix or replace the list.
    portal.begin(wifiStore);
    nodeLog.logf(espnet::LOG_WARN,
                 "wifi: nothing joined for %lu ms — opening the setup WiFi '%s' for a window",
                 (unsigned long)WIFI_FALLBACK_SETUP_MS, portal.ssid().c_str());
    portal.start((uint32_t)SETUP_SERVE_MAX_MS);
    portalActive = true;
  }

  flushLogs();
}
