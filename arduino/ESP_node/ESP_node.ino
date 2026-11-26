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
//   - find the relay again when its stored address stopped answering —
//     the router gave the Pi a new one (relay_find.h, task #160),
//   - report status (SSID, signal strength, heap, uptime, gpio) so the
//     dashboard shows every board's connection state,
//   - drive the settings-defined GPIO leg from the dashboard's single
//     On/Off toggle (commands arrive on the same connection),
//   - log to the relay (remote_log.h) and keep itself updated over OTA
//     (ota.h).
//
// Files: board.h, wifi_store.h, wifi_store_hw.h, setup_logic.h,
// setup_ap.h, wifi.h, net.h, ota.h, remote_log.h, relay_find.h,
// relay_find_logic.h. There is NO config file
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

// the node log is declared before everything that may report into it — the
// discovery macros below expand inside relay_find.h's own member functions
static espnet::RemoteLog nodeLog;

// discovery (task #160): report the search through the node log instead of
// the serial port — the entries are queued while the relay is unreachable
// and reach the relay's log page once the connection is back.
#define RELAY_FIND_INFO(...) nodeLog.logf(espnet::LOG_INFO, __VA_ARGS__)
#define RELAY_FIND_WARN(...) nodeLog.logf(espnet::LOG_WARN, __VA_ARGS__)
#include "relay_find.h"

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
// ── the setup WiFi's bounded windows (task #152) ──
// A factory node (no credentials anywhere) serves its setup WiFi FOREVER —
// nothing to join, the form is the only way forward. A node that HOLDS
// credentials (the reboot right after a save, an armed maintenance window)
// gets a bounded window instead, then hands back to the normal flow: the
// first connect is what completes the install, and a forever-portal would
// capture the node so the saved network is never even tried. An
// unprovisioned node whose joins keep failing reopens the portal in these
// windows on its own (its only interface is the portal — the dashboard is
// unreachable until the first connect).
#ifndef SETUP_SERVE_MAX_MS
#define SETUP_SERVE_MAX_MS 60000  // one bounded portal window
#endif
#ifndef SETUP_RETRY_WINDOW_MS
#define SETUP_RETRY_WINDOW_MS 60000  // pause between windows while joins keep failing
#endif
// ── auto relay discovery (task #160) ──
// When the relay is not reachable at the stored address (the router gave the
// Pi a new one), the node searches for it the way espcamsys2's cameras do: a
// UDP probe the relay answers only if it knows the fleet key. The UDP port
// is ESPNET_DISCOVERY_PORT (relay_find.h; must equal the relay's
// discoveryPort, default 8083). These four knobs space the searches out: the
// first one 10 s after the search arms, then one every 60 s while the relay
// answers where it was, and after a failed search a retry after 15 s,
// doubling up to 5 min (a sweep is not free).
#ifndef ESPNET_FIND_FIRST_S
#define ESPNET_FIND_FIRST_S 10
#endif
#ifndef ESPNET_FIND_OK_S
#define ESPNET_FIND_OK_S 60
#endif
#ifndef ESPNET_FIND_RETRY_MIN_S
#define ESPNET_FIND_RETRY_MIN_S 15
#endif
#ifndef ESPNET_FIND_RETRY_MAX_S
#define ESPNET_FIND_RETRY_MAX_S 300
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
// bootPortalDecision (setup_logic.h): it opens at boot exactly when there is
// nothing to join with (factory), no relay configuration, or the operator
// armed SETUP_MODE — a factory node then serves FOREVER until the form saves
// (the save also disarms). A node holding credentials and a relay
// configuration boots into the NORMAL flow (task #152: the reboot after a
// save starts joining at once); while it has never connected it reopens the
// portal in bounded windows if the joins keep failing. A provisioned node on
// a WiFi outage just retries: no captive portal reappears where anyone
// nearby could join it.
static espnet::SetupPortal portal;
static bool portalActive = false;
static uint32_t portalClosedMs = 0;  // when the last window handed back (#152)

// ── identity: "esp-XXXXXXXX" derived from the chip id (unique out of the
// box) — nothing to edit anywhere (#140). ──
static String deviceAuto;
static const char* DEVICE = nullptr; // filled in setup()

static const char* FW = FW_VERSION;

// ── runtime state ──
static espnet::RelayConnection relay;
static espnet::OtaUpdater ota;
static espnet::WifiMulti wifiMulti;

// ── auto relay discovery (task #160) ──
// Owns nothing at construction: the search only runs when tick() is called
// from loop() with the relay connection down, and the timings come from the
// build defaults above. A found address is stored into the flash relay
// configuration (host only; port and key ride along) and the sketch
// restarts so the connection and the OTA client both pick it up.
static RelayFind relayFind(relayConfigStore, relayCfg,
                           ESPNET_DISCOVERY_PORT,
                           ESPNET_FIND_FIRST_S * 1000UL,
                           ESPNET_FIND_OK_S * 1000UL,
                           ESPNET_FIND_RETRY_MIN_S * 1000UL,
                           ESPNET_FIND_RETRY_MAX_S * 1000UL);

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
  // Why the node (re)started + how much room it has (task #152): a save that
  // resets the node mid-request shows here as "Exception" / "Hardware
  // Watchdog" / "Software Watchdog" instead of "Software/System restart".
  nodeLog.logf(espnet::LOG_INFO, "boot: reset reason '%s', free heap %u, lowest free stack %u",
               espnet::resetReason().c_str(), (unsigned)ESP.getFreeHeap(), (unsigned)espnet::freeStackBytes());

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

  // The portal object is always prepared (it loads the stored rows and the
  // setup SSID): the loop may have to open it later, see below.
  portal.begin(wifiStore, relayConfigStore);

  // The boot decision (setup_logic.h, desktop-tested): the setup WiFi opens
  // at boot when there is nothing to join with, no relay configuration, or
  // SETUP_MODE is armed. Credentials + relay configuration = the normal
  // flow, also right after a save (task #152).
  const bool portalWanted =
      espnet::bootPortalDecision(wifiStore.setupArmed(), effectiveCount, relayConfigured) ==
      espnet::PortalDecision::Portal;
  if (portalWanted) {
    nodeLog.logf(espnet::LOG_WARN,
                 "setup needed (credentials=%d relay=%d armed=%d provisioned=%d) - opening the setup WiFi; "
                 "join '%s' (password in INSTALL.md) and pick the network(s) + relay there",
                 effectiveCount, relayConfigured ? 1 : 0, wifiStore.setupArmed() ? 1 : 0,
                 provisionedDone ? 1 : 0, portal.ssid().c_str());
    const uint32_t serveCapMs = espnet::bootPortalServeCapMs(effectiveCount == 0, SETUP_SERVE_MAX_MS);
    nodeLog.logf(espnet::LOG_INFO, "setup: portal window %s",
                 serveCapMs == 0 ? "unbounded (factory node, serves until saved)" : "bounded (the node holds credentials)");
    portal.start(serveCapMs); // factory: forever; holds credentials: bounded window (#152)
    portalActive = true;
  } else if (!provisionedDone) {
    // Saved credentials + relay, never connected yet (the reboot after the
    // setup form's save): join FIRST. If no join succeeds for a whole retry
    // window the loop reopens the portal (the timer below), so a wrong
    // password can still be fixed from the phone.
    portalClosedMs = millis();
    nodeLog.logf(espnet::LOG_INFO,
                 "setup saved - joining %d stored network(s), relay %s:%d; the setup WiFi reopens in %lu s if no join succeeds",
                 effectiveCount, relayCfg.host.c_str(), relayCfg.port, (unsigned long)(SETUP_RETRY_WINDOW_MS / 1000));
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
      nodeLog.logf(espnet::LOG_INFO, "setup: saved - restarting now (lowest free stack %u)",
                   (unsigned)espnet::freeStackBytes());
      Serial.flush();  // let the line out before the reset
      ESP.restart(); // the saved credentials take over (session stopped the AP)
      return;
    }
    if (step == espnet::SetupStep::Handback) {
      // The bounded window expired (#152): resume the normal flow so the
      // saved credentials are actually tried — this is what completes the
      // first install (or reopens the window below when no join ever
      // succeeds on an unprovisioned node).
      portalActive = false;
      portalClosedMs = millis();
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
    } else if (relayConfigured) {
      // #160: the relay is not answering at the stored address (the router
      // gave the Pi a new one). The search runs on its own schedule — nothing
      // while the connection works, then a probe to the stored address, a
      // broadcast, a sweep of the subnet — and when the relay proves it knows
      // the fleet key from ANOTHER address, the new host is stored in flash
      // and the node restarts into it. A healthy system never sends a packet.
      if (relayFind.tick(relayCfg.host.c_str(), relayCfg.key.c_str(), true)) {
        nodeLog.logf(espnet::LOG_WARN, "relay moved - restarting to use %s", relayCfg.host.c_str());
        Serial.flush();  // let the line out before the reset
        ESP.restart();
        return;
      }
    }
    ota.tick(FW);
  }

  // #152: the first install is still incomplete (credentials saved but no
  // join ever succeeded) and no portal is up: reopen the setup WiFi in
  // bounded windows so a wrong password can always be fixed from the phone.
  // The portal and the join attempts alternate — a scan or connect while the
  // AP serves would switch the radio's channel and kill the phone's session
  // mid-form. A provisioned node never comes back here: its outage just
  // retries, re-entry is deliberate (arm SETUP_MODE from the dashboard).
  if (!provisionedDone && portalClosedMs != 0 &&
      static_cast<uint32_t>(millis() - portalClosedMs) >= SETUP_RETRY_WINDOW_MS) {
    portalClosedMs = 0; // one window per pause; the next handback restarts the cycle
    portal.start(SETUP_SERVE_MAX_MS);
    portalActive = true;
    nodeLog.logf(espnet::LOG_WARN,
                 "wifi: still not provisioned - reopening the setup WiFi '%s' for a bounded window",
                 portal.ssid().c_str());
  }
  flushLogs();
}
