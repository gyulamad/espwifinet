// ESP_node.ino — one node of the ESP Wifi Network (task #33 base
// project, #36 setup & settings). Role of this sketch:
//   - join WiFi through the multi-network manager (wifi.h): always
//     (re)connects to the STRONGEST of the networks listed in config.h,
//   - keep one OUTBOUND connection to the relay (works behind WiFi
//     extenders / NAT routers — see net.h),
//   - report status (SSID, signal strength, heap, uptime, gpio) so the
//     dashboard shows every board's connection state,
//   - drive the settings-defined GPIO leg from the dashboard's single
//     On/Off toggle (commands arrive on the same connection),
//   - log to the relay (remote_log.h) and keep itself updated over OTA
//     (ota.h).
//
// Files: config.h (edit per install), board.h, wifi.h, net.h, ota.h, remote_log.h.
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
#include "wifi.h"

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

// The known networks (config.h) — unused slots keep an empty ssid and are
// skipped by the strongest-network picker (wifi.h).
static espnet::WifiNetwork NETWORKS[] = {
    {WIFI_NETWORK_1_SSID, WIFI_NETWORK_1_PASS},
    {WIFI_NETWORK_2_SSID, WIFI_NETWORK_2_PASS},
    {WIFI_NETWORK_3_SSID, WIFI_NETWORK_3_PASS},
};

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

  wifiMulti.begin(NETWORKS, (int)(sizeof(NETWORKS) / sizeof(NETWORKS[0])), &nodeLog);
  relay.begin(RELAY_HOST, RELAY_PORT, DEVICE, NODE_KEY, &nodeLog, makeHooks());
  ota.begin(RELAY_HOST, RELAY_PORT, &nodeLog);
}

void loop() {
  espnet::feedWdt();

  wifiMulti.ensure(); // scan → pick the strongest known network → connect

  if (wifiMulti.connected()) {
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

  flushLogs();
}
