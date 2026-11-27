// test_net_logic.cpp — compiles the node's real headers (net.h, ota.h,
// remote_log.h) against the stub Arduino environment and tests the parts
// that run identical on hardware: the flat-JSON readers, the wire framing
// (bytes identical to nodejs/esp-relay/lib/protocol.js), message dispatch
// into the hooks, and the log queue. The radio paths and the real flashing
// stay hardware-test-only — the desktop build walks the flash FAILURE path
// against the per-board flasher stubs (task #165).

#include <cassert>
#include <cstdio>
#include <string>

#include "../stubs/WiFiClient.h" // pulls in Arduino.h + the radio/HTTP stubs
#include "../stubs/EEPROM.h"     // the credential store's flash surface (#135)
// The flasher stub rides the board the build targets (task #165): ota.h
// includes the ESP32 core's <Update.h> or the ESP8266 core's <Updater.h>,
// and the flow below drives whichever object the compiled board carries.
#if defined(ESP32)
#include "../stubs/Update.h"
using FlasherStub = UpdateStub;
#else
#include "../stubs/Updater.h"
using FlasherStub = UpdaterStub;
#endif

// stub singletons the headers reference
std::string Stream::serialOut;
static EspStub espInstance;
static WiFiStub wifiInstance;
EspStub& ESP = espInstance;
WiFiStub& WiFi = wifiInstance;
#if defined(ESP32)
static UpdateStub updateInstance;
UpdateStub Update = updateInstance;
#else
static UpdaterStub updaterInstance;
UpdaterStub Update = updaterInstance;   // the ESP8266 core names its flasher object Update (Updater.h)
#endif
Stream Serial;         // the real core's global port (stub Arduino.h declares it)
EEPROMClass EEPROM;  // the credential store's flash region (#135)
static unsigned long fakeNow = 1000;
unsigned long millis() { return fakeNow; }
void delay(unsigned long) {}

#include "../../arduino/ESP_node/remote_log.h"
#include "../../arduino/ESP_node/net.h"
#include "../../arduino/ESP_node/wifi.h"
#include "../../arduino/ESP_node/ota.h"
#include "../../arduino/ESP_node/wifi_store.h"
#include "../../arduino/ESP_node/setup_logic.h"

// ── #131: the board selection resolved (one build per board, same sources).
// The desktop build defines ESP8266 or ESP32 explicitly; on hardware the
// core itself defines the macro. board.h carries the per-board differences:
// chip name for the relay hello, the OTA image path, the GPIO ceiling (the
// ESP32 additionally refuses its flash pins 6..11).
static void assertBoardSelection() {
#if defined(ESP32)
  assert(std::string(ESPNET_CHIP_NAME) == "esp32");
  assert(std::string(ESPNET_FIRMWARE_PATH) == "/firmware/node-esp32.bin");
  assert(espnet::gpioPinAllowed(16));
  assert(espnet::gpioPinAllowed(33));
  assert(!espnet::gpioPinAllowed(34));
  assert(!espnet::gpioPinAllowed(6) && !espnet::gpioPinAllowed(11)); // flash pins
  assert(!espnet::gpioPinAllowed(-1));
  // The ESP32 desktop build must ride the real ESP32 core's surface: no
  // min()/max() macros (the real core resolves them to std::min), and the
  // reset reason comes from esp_reset_reason() - the stub steerable through
  // espResetReasonState() - because EspClass has no getResetReason() there
  // (the exact API mismatch the real esp32 compile caught in v1, #131 v2).
#ifdef min
#error "the ESP32 desktop build must not define the min() macro (the real ESP32 core does not)"
#endif
  espResetReasonState() = ESP_RST_POWERON;
  assert(std::string(espnet::resetReason().c_str()) == "Power on");
  espResetReasonState() = ESP_RST_TASK_WDT;
  assert(std::string(espnet::resetReason().c_str()) == "Task Watchdog");
  espResetReasonState() = ESP_RST_BROWNOUT;
  assert(std::string(espnet::resetReason().c_str()) == "Brownout");
  espResetReasonState() = ESP_RST_UNKNOWN;
  assert(std::string(espnet::resetReason().c_str()) == "Unknown");
#else // ESP8266
  assert(std::string(ESPNET_CHIP_NAME) == "esp8266");
  assert(std::string(ESPNET_FIRMWARE_PATH) == "/firmware/node.bin");
  assert(espnet::gpioPinAllowed(0) && espnet::gpioPinAllowed(16));
  assert(!espnet::gpioPinAllowed(17));
  assert(!espnet::gpioPinAllowed(-1));
  assert(std::string(espnet::resetReason().c_str()) == "External System"); // the stub's word
#endif
}

// ── helpers ──
static std::string framed(const std::string& json) {
  uint32_t len = (uint32_t)json.size();
  std::string out;
  out += (char)((len >> 24) & 0xff);
  out += (char)((len >> 16) & 0xff);
  out += (char)((len >> 8) & 0xff);
  out += (char)(len & 0xff);
  out += json;
  return out;
}

struct Captured {
  long settingsPin = -1;
  bool settingsActive = true;
  long settingsStatus = -1;
  long settingsMin = -2;
  int setupModeSeen = -1; // -1 = the hook never fired (no setupMode key)
  std::string gpioState;
  long gpioPin = -1;
};
static Captured g_cap; // NetHooks are C function pointers (as on hardware)

static void hookSetupMode(bool armed) {
  g_cap.setupModeSeen = armed ? 1 : 0;
}

static void hookSettings(long pin, bool active, long ms, long minLevel) {
  g_cap.settingsPin = pin;
  g_cap.settingsActive = active;
  g_cap.settingsStatus = ms;
  g_cap.settingsMin = minLevel;
}
static void hookSetGpio(const char* state, long pin, bool) {
  g_cap.gpioState = state;
  g_cap.gpioPin = pin;
}

static void testJsonReaders() {
  const char* settings = "{\"type\":\"settings\",\"gpioPin\":14,\"gpioActiveHigh\":false,\"statusIntervalMs\":30000,\"minLogLevel\":\"warn\"}";
  char str[32];
  long num = 0;
  bool flag = true;

  assert(espnet::jsonGetString(settings, "type", str, sizeof(str)));
  assert(std::string(str) == "settings");
  assert(espnet::jsonGetInt(settings, "gpioPin", num) && num == 14);
  assert(espnet::jsonGetInt(settings, "statusIntervalMs", num) && num == 30000);
  assert(espnet::jsonGetBool(settings, "gpioActiveHigh", flag) && flag == false);
  assert(espnet::jsonGetString(settings, "minLogLevel", str, sizeof(str)));
  assert(std::string(str) == "warn");

  const char* gpio = "{\"type\":\"set_gpio\",\"state\":\"on\",\"pin\":5,\"activeHigh\":true}";
  assert(espnet::jsonGetString(gpio, "state", str, sizeof(str)));
  assert(std::string(str) == "on");
  assert(espnet::jsonGetInt(gpio, "pin", num) && num == 5);
  assert(espnet::jsonGetBool(gpio, "activeHigh", flag) && flag == true);

  // absent keys must be reported, not guessed
  assert(!espnet::jsonGetString(gpio, "missing", str, sizeof(str)));
  assert(!espnet::jsonGetInt(gpio, "missing", num));
  assert(!espnet::jsonGetBool(gpio, "missing", flag));
}

static void testLogQueue() {
  espnet::RemoteLog lg;
  lg.begin(nullptr); // no serial echo needed here

  lg.logf(espnet::LOG_INFO, "first %d", 1);
  lg.logf(espnet::LOG_WARN, "second");
  assert(lg.pending());

  espnet::LogEntry e;
  assert(lg.pop(e) && std::string(e.msg) == "first 1" && e.level == espnet::LOG_INFO);
  assert(lg.pop(e) && std::string(e.msg) == "second" && e.level == espnet::LOG_WARN);
  assert(!lg.pending());

  // repeat suppression: identical message inside 5 s collapses to one + summary
  lg.logf(espnet::LOG_WARN, "wifi lost");
  for (int i = 0; i < 4; i++) {
    fakeNow += 1000;
    lg.logf(espnet::LOG_WARN, "wifi lost");
  }
  // the summary is emitted when the NEXT different message arrives
  lg.logf(espnet::LOG_INFO, "moved on");
  assert(lg.pop(e) && std::string(e.msg) == "wifi lost");
  assert(lg.pop(e) && std::string(e.msg).find("(repeated 4 more times)") != std::string::npos);
  assert(lg.pop(e) && std::string(e.msg) == "moved on");

  // bounded queue: the oldest entries give way once LOG_CAPACITY is exceeded
  lg.begin(nullptr);
  for (size_t i = 0; i < espnet::LOG_CAPACITY + 4; i++) {
    lg.logf(espnet::LOG_INFO, "entry %zu", i);
    fakeNow += 6000; // defeat repeat suppression
  }
  size_t seen = 0;
  while (lg.pop(e)) seen++;
  assert(seen == espnet::LOG_CAPACITY); // never more than the ring holds
  assert(lg.dropped() == 4);
}

static void testFraming() {
  // The node's outgoing framing must match the relay's protocol.js exactly:
  // [4-byte big-endian length][JSON bytes]
  const std::string json = "{\"type\":\"pong\"}";
  WiFiClient client;
  espnet::sendMessage(client, json.c_str());
  const std::string sent = client.sentAndClear();
  assert(sent == framed(json));
  const unsigned char* raw = reinterpret_cast<const unsigned char*>(sent.data());
  const uint32_t len = ((uint32_t)raw[0] << 24) | ((uint32_t)raw[1] << 16) | ((uint32_t)raw[2] << 8) | raw[3];
  assert(len == json.size());
}

static void testPumpDispatch() {
  espnet::RemoteLog lg;
  lg.begin(nullptr);

  g_cap = Captured{};
  espnet::NetHooks hooks;
  hooks.onSettings = hookSettings;
  hooks.onSetGpio = hookSetGpio;
  hooks.onSetupMode = hookSetupMode;

  WiFiClient client;
  espnet::RelayConnection conn;
  conn.begin("10.0.0.1", 7777, "esp-TEST", "key", &lg, hooks, &client);

  // The relay's real messages (same bytes the e2e suite sends over TCP).
  client.feed(framed("{\"type\":\"settings\",\"gpioPin\":14,\"gpioActiveHigh\":false,\"statusIntervalMs\":30000,\"minLogLevel\":\"error\",\"setupMode\":true}"));
  client.feed(framed("{\"type\":\"set_gpio\",\"state\":\"on\",\"pin\":14,\"activeHigh\":false}"));
  client.feed(framed("{\"type\":\"ping\"}"));

  assert(conn.pumpMessages());
  assert(g_cap.settingsPin == 14 && g_cap.settingsActive == false && g_cap.settingsStatus == 30000);
  assert(g_cap.settingsMin == 2); // the pushed log level reaches the hook
  assert(g_cap.setupModeSeen == 1); // the SETUP_MODE switch rides the settings push (#135 v2)
  assert(g_cap.gpioState == "on" && g_cap.gpioPin == 14);

  // the ping must have been answered with a pong on the same connection
  const std::string reply = client.sentAndClear();
  assert(reply.find("{\"type\":\"pong\"}") != std::string::npos);
  const unsigned char* raw = reinterpret_cast<const unsigned char*>(reply.data());
  const uint32_t len = ((uint32_t)raw[0] << 24) | ((uint32_t)raw[1] << 16) | ((uint32_t)raw[2] << 8) | raw[3];
  assert(len == reply.size() - 4);

  // split delivery: one message chopped across two reads must still parse
  const std::string whole = framed("{\"type\":\"settings\",\"gpioPin\":5,\"gpioActiveHigh\":true,\"statusIntervalMs\":10000,\"setupMode\":false}");
  client.feed(whole.substr(0, 10));
  assert(conn.pumpMessages());
  client.feed(whole.substr(10));
  assert(conn.pumpMessages());
  assert(g_cap.settingsPin == 5 && g_cap.settingsActive == true && g_cap.settingsStatus == 10000);
  assert(g_cap.settingsMin == -1); // a settings message without minLogLevel keeps the compiled level
  assert(g_cap.setupModeSeen == 0); // ... and without setupMode the hook fires with false (older relay)

  // a bad length prefix is a protocol error (above the 4096-byte sanity cap)
  std::string bad;
  bad += (char)0x00; bad += (char)0x00; bad += (char)0x10; bad += (char)0x01; // 4097
  bad += "junk";
  client.feed(bad);
  assert(!conn.pumpMessages());
}

static void testOtaCompile() {
  // Compile-and-instantiate only: the flash/radio path needs hardware.
  // fetchVersion/flashFirmware are covered by the relay e2e suite from the
  // server side (same endpoints, same reply shape).
  espnet::RemoteLog lg;
  lg.begin(nullptr);
  espnet::OtaUpdater ota;
  ota.begin("10.0.0.1", 8080, &lg);
  ota.tick("1.0.0"); // fires the version check against the stub client
  printf("      (ota tick exercised against stub http endpoints)\n");
}

static void testOtaFlashFailurePath() {
  // task #165: with the relay serving a DIFFERENT version, tick() goes all
  // the way to the flash step, which must run through the COMPILED BOARD's
  // flasher object (named Update on BOTH cores; the ESP8266 core declares it in Updater.h —
  // the per-board adapters) and must log the flasher's own error without
  // restarting. The stub starts the flash and fails the write, so every
  // adapter (begin/writeStream/end/error/drop) runs on both desktop builds.
  WiFi.wifiStatus = WL_CONNECTED;
  HTTPClient::code = 200;
  HTTPClient::size = 321;
  HTTPClient::body = String("9.9.9\ncafebabe\n");
  FlasherStub::beginSucceeds = true;
  FlasherStub::written = 0;
  FlasherStub::endSucceeds = false;

  espnet::RemoteLog lg;
  lg.begin(nullptr);
  espnet::OtaUpdater ota;
  ota.begin("10.0.0.1", 8080, &lg);
  ota.tick("1.0.0"); // relay serves 9.9.9 -> flash attempt -> stub write fails

  espnet::LogEntry e;
  bool sawWriteError = false;
  while (lg.pop(e)) {
    if (std::string(e.msg) == "ota: wrote 0/321 bytes, err stub: not on hardware") {
      sawWriteError = true;
    }
  }
  assert(sawWriteError); // the board's flasher error getter landed in the log

  // restore the defaults the other tests rely on
  HTTPClient::code = 404;
  HTTPClient::size = -1;
  HTTPClient::body = String();
  FlasherStub::beginSucceeds = false;
  FlasherStub::written = 0;
  FlasherStub::endSucceeds = false;
}

static void testPickStrongest() {
  String ss[] = {String("a"), String("b"), String("c"), String("b")};
  long rs[] = {-60, -50, -40, -45};
  const espnet::WifiNetwork k1[] = {{"a", "p"}};
  const espnet::WifiNetwork k2[] = {{"a", "p"}, {"b", "p"}, {"c", "p"}};
  const espnet::WifiNetwork k3[] = {{"", ""}, {"b", "p"}}; // slot 0 unused

  // only KNOWN networks count: c (-40) is the strongest overall but unknown
  // to k1, so a wins there; with the full list c wins regardless of order
  assert(espnet::pickStrongestNetwork(ss, rs, 4, k1, 1) == 0);
  assert(espnet::pickStrongestNetwork(ss, rs, 4, k2, 3) == 2);

  // ties resolve to the first matching known slot (stable choice)
  String tss[] = {String("b"), String("a")};
  long trs[] = {-55, -55};
  const espnet::WifiNetwork k4[] = {{"b", "p"}, {"a", "p"}};
  assert(espnet::pickStrongestNetwork(tss, trs, 2, k4, 2) == 0);

  // unused slots (empty ssid) never match
  assert(espnet::pickStrongestNetwork(ss, rs, 4, k3, 2) == 1);

  // nothing visible / empty scan
  String ess[] = {String("x")};
  long ers[] = {-30};
  assert(espnet::pickStrongestNetwork(ess, ers, 1, k2, 3) == -1);
  assert(espnet::pickStrongestNetwork(ess, ers, 0, k2, 3) == -1);
}

// ── wifi.h: the scan → pick strongest → connect cycle (stubbed radio) ──
static void resetWifiStub() {
  WiFi.wifiStatus = WL_DISCONNECTED;
  WiFi.lastBeginSsid.clear();
  WiFi.scanCount = 0;
  WiFi.scanState = 3; // idle
  WiFi.connectedSsid = String("");
  WiFi.connectedRssi = 0;
}

static void testWifiMulti() {
  espnet::RemoteLog lg;
  lg.begin(nullptr);
  const espnet::WifiNetwork nets[] = {{"alpha", "pass-a"}, {"beta", "pass-b"}, {"", ""}};

  fakeNow = 1000;
  resetWifiStub();
  espnet::WifiMulti multi;
  multi.begin(nets, 3, &lg);
  assert(!multi.connected());
  multi.ensure(); // backoff holds on the first pass
  assert(std::string(multi.phaseName()) == "disconnected");
  assert(WiFi.scanState == 3); // nothing started yet

  // after the backoff a scan starts; the air shows alpha -70, beta -50,
  // gamma -40 (unknown — must be ignored)
  fakeNow += 5000;
  const char* seen[] = {"alpha", "beta", "gamma"};
  long seenRssi[] = {-70, -50, -40};
  WiFi.setScanResults(seen, seenRssi, 3);
  multi.ensure();
  assert(std::string(multi.phaseName()) == "scanning");
  WiFi.finishScan();
  multi.ensure(); // results in → beta (strongest KNOWN, not gamma)
  assert(std::string(multi.phaseName()) == "connecting");
  assert(WiFi.lastBeginSsid == "beta");

  WiFi.connectedSsid = String("beta");
  WiFi.connectedRssi = -50;
  WiFi.wifiStatus = WL_CONNECTED;
  multi.ensure();
  assert(multi.connected());
  assert(multi.currentNetwork() == 1);

  // healthy link: the roam re-evaluation must NOT churn (no scan started)
  WiFi.scanState = 3;
  fakeNow += 30000;
  multi.ensure();
  assert(std::string(multi.phaseName()) == "online");
  assert(WiFi.scanState == 3);

  // degraded link (-82 dBm): the next roam check starts a scan
  WiFi.connectedRssi = -82;
  fakeNow += 30000;
  multi.ensure();
  assert(std::string(multi.phaseName()) == "scanning");

  // alpha is now -70 while beta (the current AP) has also degraded to -82
  // in the air: alpha beats it by 12 dB → hand-over
  const char* seen2[] = {"alpha", "beta", "gamma"};
  long seen2Rssi[] = {-70, -82, -40};
  WiFi.setScanResults(seen2, seen2Rssi, 3);
  WiFi.finishScan();
  multi.ensure();
  assert(std::string(multi.phaseName()) == "connecting");
  assert(WiFi.lastBeginSsid == "alpha");

  WiFi.connectedSsid = String("alpha");
  WiFi.connectedRssi = -70;
  WiFi.wifiStatus = WL_CONNECTED;
  multi.ensure();
  assert(multi.connected() && multi.currentNetwork() == 0);

  // a roam that finds nothing better keeps the current link
  WiFi.scanState = 3;
  WiFi.connectedRssi = -82;
  fakeNow += 30000;
  multi.ensure(); // weak → roam scan
  assert(std::string(multi.phaseName()) == "scanning");
  const char* weak[] = {"beta"};
  long weakRssi[] = {-80}; // only 2 dB better than -82 → below the margin
  WiFi.setScanResults(weak, weakRssi, 1);
  WiFi.finishScan();
  multi.ensure();
  assert(multi.connected());
  assert(std::string(multi.phaseName()) == "online");

  // connection lost while online → back to reconnecting
  WiFi.wifiStatus = WL_DISCONNECTED;
  multi.ensure();
  assert(std::string(multi.phaseName()) == "disconnected");
  WiFi.scanState = 3;
  fakeNow += 5000;
  const char* strangers[] = {"gamma"};
  long strangersRssi[] = {-30};
  WiFi.setScanResults(strangers, strangersRssi, 1);
  multi.ensure();
  WiFi.finishScan();
  multi.ensure();
  assert(std::string(multi.phaseName()) == "disconnected"); // nothing known visible
  assert(WiFi.lastBeginSsid != "gamma");
}

static void testMinLogFilter() {
  static Stream ser; // echo capture for the serial path
  espnet::RemoteLog lg;
  lg.begin(&ser);
  lg.setMinLevel(espnet::LOG_WARN); // relay pushed "warn": info stays local

  Stream::serialOut.clear();
  lg.logf(espnet::LOG_INFO, "chatty info");
  assert(!lg.pending()); // filtered info must not be queued
  assert(Stream::serialOut.find("chatty info") != std::string::npos); // but serial sees it
  lg.logf(espnet::LOG_ERROR, "real problem");
  assert(lg.pending());
  espnet::LogEntry e;
  assert(lg.pop(e) && std::string(e.msg) == "real problem" && e.level == espnet::LOG_ERROR);
  assert(!lg.pending());
}

// ── task #135: the flash-held WiFi credential store (wifi_store.h) ──

class RamStoreBackend : public espnet::WifiStoreBackend {
 public:
  bool read(char* buf, size_t max) override {
    if (blank_ || max < data_.size()) return false;
    for (size_t i = 0; i < data_.size(); ++i) buf[i] = data_[i];
    return true;
  }
  bool write(const char* buf, size_t len) override {
    if (failWrite_) return false;
    data_.assign(buf, len);
    blank_ = false;
    return true;
  }
  std::string data_;
  bool blank_ = true;
  bool failWrite_ = false;
};

static void testWifiStore() {
  using espnet::WifiStoreNet;
  WifiStoreNet nets[espnet::WIFI_STORE_MAX_NETS];
  char blob[espnet::WIFI_STORE_REGION_MAX];

  // round-trip, open network included
  nets[0].ssid = "Home";
  nets[0].pass = "secret12";
  nets[1].ssid = "Cafe";
  nets[1].pass = ""; // open
  const int len = espnet::wifiStoreSerialize(nets, 2, blob, sizeof(blob));
  assert(len > 0);
  WifiStoreNet back[espnet::WIFI_STORE_MAX_NETS];
  assert(espnet::wifiStoreParse(blob, (size_t)len, back, espnet::WIFI_STORE_MAX_NETS) == 2);
  assert(back[0].ssid == "Home" && back[0].pass == "secret12");
  assert(back[1].ssid == "Cafe" && back[1].pass.empty());

  // validation refuses what the radio or the framing could not carry
  std::string longSsid(33, 'a');
  nets[0].ssid = longSsid.c_str();
  assert(espnet::wifiStoreSerialize(nets, 2, blob, sizeof(blob)) < 0);
  std::string tabSsid = "a\tb";
  nets[0].ssid = tabSsid.c_str();
  assert(espnet::wifiStoreSerialize(nets, 2, blob, sizeof(blob)) < 0);
  std::string shortPass = "1234567";
  nets[0].ssid = "Home";
  nets[0].pass = shortPass.c_str();
  assert(espnet::wifiStoreSerialize(nets, 2, blob, sizeof(blob)) < 0);
  std::string longPass(64, 'x');
  nets[0].pass = longPass.c_str();
  assert(espnet::wifiStoreSerialize(nets, 2, blob, sizeof(blob)) < 0);
  std::string nlPass = "abc\ndef";
  nets[0].pass = nlPass.c_str();
  assert(espnet::wifiStoreSerialize(nets, 2, blob, sizeof(blob)) < 0);
  nets[0].pass = "secret12";

  // the parser is all-or-nothing: garbage, a cut-off entry, a bad entry, a
  // capacity overflow all answer 0 (the compiled list takes over)
  assert(espnet::wifiStoreParse("GARBAGE", 7, back, espnet::WIFI_STORE_MAX_NETS) == 0);
  assert(espnet::wifiStoreParse("", 0, back, espnet::WIFI_STORE_MAX_NETS) == 0);
  assert(espnet::wifiStoreParse(blob, (size_t)len - 1, back, espnet::WIFI_STORE_MAX_NETS) == 0); // cut-off
  std::string badEntry = std::string("Home\t1234567\n"); // 7-char pass inside the blob
  assert(espnet::wifiStoreParse(badEntry.data(), badEntry.size(), back, espnet::WIFI_STORE_MAX_NETS) == 0);
  std::string six = std::string("a1\tp1\na2\tp2\na3\tp3\na4\tp4\na5\tp5\na6\tp6\n");
  assert(espnet::wifiStoreParse(six.data(), six.size(), back, espnet::WIFI_STORE_MAX_NETS) == 0); // > 5 slots

  // the store: save+load round-trip, blank and damaged regions, failed write
  RamStoreBackend backend;
  espnet::WifiStore store;
  store.begin(&backend);
  WifiStoreNet rows[espnet::WIFI_STORE_MAX_NETS];
  rows[0].ssid = "Home";
  rows[0].pass = "secret12";
  assert(store.load(rows, espnet::WIFI_STORE_MAX_NETS) == 0); // blank region
  assert(store.save(nets, 2));
  assert(store.load(back, espnet::WIFI_STORE_MAX_NETS) == 2);
  assert(back[0].ssid == "Home" && back[1].pass.empty());
  backend.data_ = "totally-garbage-region-bytes"; // wrong magic
  assert(store.load(back, espnet::WIFI_STORE_MAX_NETS) == 0);
  backend.failWrite_ = true;
  assert(!store.save(nets, 2)); // nothing was changed
  // #171: the failure names its cause (optional out-param, the 2-arg form
  // keeps working for callers that do not care)
  espnet::StoreWriteError why = espnet::StoreWriteError::None;
  assert(!store.save(nets, 2, &why));
  assert(why == espnet::StoreWriteError::WriteFailed);
  backend.failWrite_ = false;
  // save refuses invalid input before touching the backend
  WifiStoreNet bad[1];
  bad[0].ssid = "Home";
  bad[0].pass = "short";
  assert(!store.save(bad, 1));
  assert(!store.save(bad, 1, &why));
  assert(why == espnet::StoreWriteError::BadInput);
  assert(store.save(nets, 2, &why));
  assert(why == espnet::StoreWriteError::None);
  // a store that never began has no backend to write through
  espnet::WifiStore unbegun;
  assert(!unbegun.save(nets, 1, &why));
  assert(why == espnet::StoreWriteError::NoBackend);
  // the cause texts exist for every reason (the form embeds them in its
  // serial lines and on the 500 page)
  assert(std::string(espnet::storeWriteErrorText(espnet::StoreWriteError::None)) == "no error");
  assert(std::string(espnet::storeWriteErrorText(espnet::StoreWriteError::WriteFailed)) ==
         "the flash write did not persist");
  assert(std::string(espnet::storeWriteErrorText(espnet::StoreWriteError::BadInput)) ==
         "a value did not pass validation");

  // #138: the store holds 30 networks (the flash sector's cap) - a full
  // round-trip at capacity, byte-for-byte back
  WifiStoreNet many[espnet::WIFI_STORE_MAX_NETS];
  for (int i = 0; i < espnet::WIFI_STORE_MAX_NETS; ++i) {
    char n[8];
    snprintf(n, sizeof(n), "net%d", i + 1);
    many[i].ssid = n;
    many[i].pass = std::string("pw-") + n + "-0123456789012345678901234567890123456789" + n; // 52 chars, inside the WPA2 bound
  }
  assert(store.save(many, espnet::WIFI_STORE_MAX_NETS));
  WifiStoreNet backMany[espnet::WIFI_STORE_MAX_NETS];
  assert(store.load(backMany, espnet::WIFI_STORE_MAX_NETS) == espnet::WIFI_STORE_MAX_NETS);
  for (int i = 0; i < espnet::WIFI_STORE_MAX_NETS; ++i) {
    assert(backMany[i].ssid == many[i].ssid && backMany[i].pass == many[i].pass);
  }
}

// ── task #140: the relay configuration store ──
static void testRelayConfigStore() {
  RamStoreBackend backend;
  espnet::RelayConfigStore store;
  store.begin(&backend);

  // factory: blank region reads as "not configured"
  espnet::RelayConfig cfg;
  assert(!store.load(cfg));

  // a valid save round-trips (host, port, key) and reads back set
  cfg.host = "192.168.1.10";
  cfg.port = 7777;
  cfg.key = "fleet-key-123";
  assert(store.save(cfg));
  espnet::RelayConfig back;
  assert(store.load(back));
  assert(back.host == "192.168.1.10" && back.port == 7777 && back.key == "fleet-key-123");

  // validation refuses BEFORE any write (all-or-nothing)
  espnet::RelayConfig bad = cfg;
  espnet::StoreWriteError rwhy = espnet::StoreWriteError::None;
  bad.host = "";
  assert(!store.save(bad));
  assert(!store.save(bad, &rwhy)); // #171: the cause rides along
  assert(rwhy == espnet::StoreWriteError::BadInput);
  bad.host = "192.168.1.10";
  bad.port = 0;
  assert(!store.save(bad));
  bad.port = 7777;
  bad.key = "with\ttab";  // a tab would break the payload framing
  assert(!store.save(bad));
  back = espnet::RelayConfig();
  assert(store.load(back) && back.key == "fleet-key-123"); // nothing changed
  // a failed write names itself (task #171)
  backend.failWrite_ = true;
  assert(!store.save(cfg, &rwhy));
  assert(rwhy == espnet::StoreWriteError::WriteFailed);
  backend.failWrite_ = false;
  assert(store.save(cfg, &rwhy));
  assert(rwhy == espnet::StoreWriteError::None);

  // damaged region reads as "not configured" (which means: the portal)
  std::string damaged = "ENRC";
  damaged += static_cast<char>(0x01);
  damaged += static_cast<char>(0x00);
  damaged += static_cast<char>(0x30);
  damaged += std::string("192.168.1.10\tnot-a-number\tfleet-key-123");
  RamStoreBackend dBackend;
  espnet::RelayConfigStore dStore;
  dStore.begin(&dBackend);
  dBackend.data_ = damaged;
  dBackend.blank_ = false;
  assert(!dStore.load(back));

  // clear() blanks the region: the next boot opens the setup WiFi
  assert(store.clear());
  assert(!store.load(back));
}

// ── task #135 v2: the life-cycle flags and the v1 migration ──
static void testWifiStoreV2() {
  using espnet::WifiStoreNet;
  RamStoreBackend backend;
  espnet::WifiStore store;
  store.begin(&backend);

  // a fresh save from the setup page: v2 blob, NOT provisioned (the flag
  // flips on the first CONNECT, not on the save), not armed
  WifiStoreNet nets[1];
  nets[0].ssid = "Home";
  nets[0].pass = "secret12";
  assert(store.save(nets, 1));
  WifiStoreNet back[espnet::WIFI_STORE_MAX_NETS];
  assert(store.load(back, espnet::WIFI_STORE_MAX_NETS) == 1);
  assert(!store.provisioned() && !store.setupArmed());

  // arm (SETUP_MODE): entries survive the flag write; the save that follows
  // — a provisioning event — clears the armed flag and keeps the entries
  assert(store.armSetup(true) && store.setupArmed());
  assert(store.load(back, espnet::WIFI_STORE_MAX_NETS) == 1);
  assert(store.setupArmed());
  assert(store.save(nets, 1));
  assert(!store.setupArmed());
  assert(!store.provisioned());

  // provision (the first successful WiFi connect): persisted at once, so a
  // power cut right after cannot resurrect the setup WiFi
  assert(store.markProvisioned() && store.provisioned());
  assert(store.load(back, espnet::WIFI_STORE_MAX_NETS) == 1 && store.provisioned());
  assert(!store.setupArmed());

  // a v1 ("ENW1") region migrates on load: entries arrive provisioned (they
  // were saved through the setup page, so the node connected with them)
  RamStoreBackend v1Backend;
  espnet::WifiStore v1Store;
  v1Store.begin(&v1Backend);
  const char* blob = "Home\tsecret12\n";
  const size_t blen = 14;
  std::string region = "ENW1";
  region += static_cast<char>((blen >> 8) & 0xff);
  region += static_cast<char>(blen & 0xff);
  region += blob;
  v1Backend.data_ = region;
  v1Backend.blank_ = false; // the region exists (as a real flash erase would leave it readable)
  assert(v1Store.load(back, espnet::WIFI_STORE_MAX_NETS) == 1);
  assert(back[0].ssid == "Home" && back[0].pass == "secret12");
  assert(v1Store.provisioned());
  assert(!v1Store.setupArmed());
  // a flag write migrates the region to v2 in place, entries intact
  assert(v1Store.markProvisioned());
  assert(v1Store.load(back, espnet::WIFI_STORE_MAX_NETS) == 1);
  assert(v1Store.provisioned());

  // damaged entries read as "no credentials" with CLEAN flags (no stale
  // provisioned/armed state can survive a corrupt blob)
  std::string damaged = "ENW2";
  damaged += static_cast<char>(0x03); // provisioned + armed in the header
  damaged += static_cast<char>(0x00);
  damaged += static_cast<char>(0x0f);
  damaged += std::string("Home\t1234567\n"); // an invalid password inside
  RamStoreBackend dBackend;
  espnet::WifiStore dStore;
  dStore.begin(&dBackend);
  dBackend.data_ = damaged;
  dBackend.blank_ = false;
  assert(dStore.load(back, espnet::WIFI_STORE_MAX_NETS) == 0);
  assert(!dStore.provisioned() && !dStore.setupArmed());
}

// ── task #135: the setup decisions (setup_logic.h) ──

static void testSetupDecisions() {
  // the setup WiFi's name: "espnode-setup-" + the last 3 chip-id bytes,
  // lowercase hex, unique per board, no radio call needed
  std::string ssid;
  assert(espnet::setupSsid(0xA1B2C3D4u, ssid));
  assert(ssid == "espnode-setup-b2c3d4");
  assert(espnet::setupSsid(0x0000FFu, ssid) && ssid == "espnode-setup-0000ff");
  assert(ssid.size() <= espnet::WIFI_STORE_SSID_MAX);
  assert(!espnet::setupSsid(0u, ssid)); // a broken chip id must not open a nameless network

  // the CSRF token: 32 lowercase hex characters, deterministic per input,
  // different inputs differ
  const std::string t1 = espnet::csrfTokenFromRandom(0x01234567u, 0x89abcdefu, 0xdeadbeefu, 0x11223344u);
  assert(t1.size() == 32);
  for (size_t i = 0; i < t1.size(); ++i) {
    const bool hex = (t1[i] >= '0' && t1[i] <= '9') || (t1[i] >= 'a' && t1[i] <= 'f');
    assert(hex);
  }
  assert(t1 == espnet::csrfTokenFromRandom(0x01234567u, 0x89abcdefu, 0xdeadbeefu, 0x11223344u));
  assert(t1 != espnet::csrfTokenFromRandom(0x01234567u, 0x89abcdefu, 0xdeadbeefu, 0x11223345u));

  // store-vs-compiled decision: a valid store wins, anything damaged means
  // the compiled list, nothing anywhere means setup mode
  espnet::WifiStoreNet rows[espnet::WIFI_STORE_MAX_NETS];
  rows[0].ssid = "Home";
  rows[0].pass = "secret12";
  assert(espnet::effectiveNetworksCount(rows, 1) == 1);
  assert(espnet::effectiveNetworksCount(rows, 0) == 0);
  assert(espnet::effectiveNetworksCount(nullptr, 0) == 0);
  rows[1].ssid = "a\tb"; // one damaged row poisons the list
  rows[1].pass = "abcdefgh";
  assert(espnet::effectiveNetworksCount(rows, 2) == 0);

  // the boot decision (v2): armed always opens the setup WiFi; not
  // provisioned always opens it (factory, or saved-but-never-connected);
  // a provisioned node NEVER opens it on its own (an outage just retries)
  using espnet::PortalDecision;
  // (armed, usable networks, relay configured)
  assert(espnet::bootPortalDecision(true, 3, false) == PortalDecision::Portal);
  assert(espnet::bootPortalDecision(true, 3, true) == PortalDecision::Portal);
  assert(espnet::bootPortalDecision(false, 0, false) == PortalDecision::Portal); // factory
  assert(espnet::bootPortalDecision(false, 0, true) == PortalDecision::Portal);  // nothing to join
  // #140: a provisioned node WITHOUT a relay configuration still opens the
  // portal once — that is the migration path of a v2 node whose relay
  // settings were compiled in until now
  assert(espnet::bootPortalDecision(false, 2, false) == PortalDecision::Portal);
  // #152: credentials + relay = the normal flow. This is ALSO the reboot right
  // after the setup form's save (provisioned is still false until the first
  // connect): the node must start joining and reporting, not reopen its own
  // WiFi. A never-connected node whose joins fail gets the portal back from
  // the sketch's retry window (see testSketchBootsIntoJoinAfterSave).
  assert(espnet::bootPortalDecision(false, 2, true) == PortalDecision::Normal);

  // #152: how long a portal boot may serve. A factory node (no usable
  // credentials) serves forever — the form is its only way forward; a node
  // that holds credentials gets the bounded window so the reboot after a
  // save actually TRIES the saved network instead of being captured by its
  // own portal forever.
  assert(espnet::bootPortalServeCapMs(true, 60000) == 0);
  assert(espnet::bootPortalServeCapMs(false, 60000) == 60000);
  assert(espnet::bootPortalServeCapMs(false, 1) == 1);
}

// ── task #135: the setup form (SetupForm) ──

static const char* const kToken = "0123456789abcdef0123456789abcdef"; // 32 hex, >= WIFI_SETUP_MIN_TOKEN

static std::string saveBody(const std::string& rowFields) {
  // #140: every successful save carries the relay fields too (the node
  // cannot report anywhere without them) - valid values, so tests that
  // expect a WiFi-side refusal still see exactly that error.
  return std::string("csrf=") + kToken + "&" + rowFields +
         "&r.host=192.168.1.10&r.port=7777&r.key=fleet-key-123";
}

static void testSetupForm() {
  RamStoreBackend backend;
  espnet::WifiStore store;
  store.begin(&backend);
  espnet::WifiStoreNet stored[espnet::WIFI_STORE_MAX_NETS];
  RamStoreBackend rbe_form; espnet::RelayConfigStore rs_form; rs_form.begin(&rbe_form);
  espnet::SetupForm form("espnode-setup-aabbcc", store, stored, 0, rs_form, espnet::RelayConfig(), kToken);;

  // first visit: the page carries the token, the empty rows, no script
  espnet::HttpReply page = form.handle("GET", "/", "");
  assert(page.status == 200);
  assert(page.body.find("name=\"csrf\" value=\"" + std::string(kToken) + "\"") != std::string::npos);
  assert(page.body.find("network name 1") != std::string::npos);
  // #138: one row by default; the page references the same-origin script
  // (no inline script, no handler attributes) and offers the Add/Remove UI
  assert(page.body.find("<script src=\"/setup.js\"></script>") != std::string::npos);
  assert(page.body.find("id=\"addrow\"") != std::string::npos);
  assert(page.body.find("class=\"rm\"") != std::string::npos);
  assert(page.body.find("<noscript>") != std::string::npos); // the no-JS fallback rows
  // #140: the relay section is on the page; the key is never echoed
  assert(page.body.find("Relay (the Pi this node reports to)") != std::string::npos);
  assert(page.body.find("name=\"r.host\"") != std::string::npos);
  assert(page.body.find("name=\"r.key\"") != std::string::npos);
  // #171: the inputs carry classes (the script selects by class, not by
  // type — a Show button flips the type at runtime), and the Show/Hide
  // buttons themselves are created BY the script, so the static page and a
  // no-JavaScript browser show exactly the form it always did
  assert(page.body.find("class=\"ssid\"") != std::string::npos);
  assert(page.body.find("class=\"pass\"") != std::string::npos);
  assert(page.body.find("class=\"open\"") != std::string::npos);
  assert(page.body.find("type=\"password\" class=\"pass\" name=\"r.key\"") != std::string::npos);
  assert(page.body.find("class=\"showpw\"") == std::string::npos); // no static toggle button (the CSS style may mention it)
  assert(page.body.find("onclick=") == std::string::npos);
  assert(page.body.find("<script>") == std::string::npos);   // nothing inline
  bool hasCsp = false;
  for (size_t i = 0; i < page.headers.size(); ++i) {
    if (page.headers[i].first == "Content-Security-Policy") hasCsp = true;
    if (page.headers[i].first == "Location") assert(false); // no redirect on the form
  }
  assert(hasCsp);

  // the form's Add/Remove helper is served by the same handler (#138)
  espnet::HttpReply js = form.handle("GET", "/setup.js", "");
  assert(js.status == 200);
  assert(js.contentType == "text/javascript; charset=utf-8");
  assert(js.body.find("renumber") != std::string::npos);
  assert(js.body.find("'w.ssid.' + n") != std::string::npos);
  assert(js.body.find("rows.length >= 30") != std::string::npos);
  // #171: the Show/Hide toggle rides the same same-origin script
  assert(js.body.find("input[type=password]") != std::string::npos); // buttons attach to every password box
  assert(js.body.find(".showpw") != std::string::npos);
  assert(js.body.find("aria-pressed") != std::string::npos);
  assert(js.body.find("input.type === 'password'") != std::string::npos); // the flip
  assert(js.body.find("input.ssid") != std::string::npos &&
         js.body.find("input.pass") != std::string::npos &&
         js.body.find("input.open") != std::string::npos); // class-based renumber
  assert(js.body.find("input[type=text]") == std::string::npos); // no type-based selector left
  assert(js.body.find("resetToggle") != std::string::npos); // an added row starts hidden again
  bool jsHeaders = false;
  for (size_t i = 0; i < js.headers.size(); ++i) {
    if (js.headers[i].first == "Cache-Control" && js.headers[i].second == "no-store") jsHeaders = true;
  }
  assert(jsHeaders);
  assert(form.handle("POST", "/setup.js", "").status == 405);

  // every POST is guarded by the per-session token (constant-time compare)
  espnet::HttpReply r = form.handle("POST", "/save", "w.ssid.1=Home&w.pass.1=abcdefgh");
  assert(r.status == 403); // missing token
  r = form.handle("POST", "/save", "csrf=short&w.ssid.1=Home&w.pass.1=abcdefgh");
  assert(r.status == 403); // token shorter than the minimum
  r = form.handle("POST", "/save", "csrf=fedcba9876543210fedcba9876543210&w.ssid.1=Home&w.pass.1=abcdefgh");
  assert(r.status == 403); // wrong token
  assert(backend.blank_);  // nothing was stored by the refused posts

  // #140: a save without the relay fields is refused - the node would have
  // nowhere to report - and nothing is stored (all-or-nothing across both
  // flash regions)
  r = form.handle("POST", "/save", std::string("csrf=") + kToken + "&w.ssid.1=Home&w.pass.1=abcdefgh");
  assert(r.status == 422);
  assert(r.body.find("Relay address") != std::string::npos);
  assert(backend.blank_);

  // a valid save: all-or-nothing, BOTH stores are written, the node restarts
  r = form.handle("POST", "/save", saveBody("w.ssid.1=Home&w.pass.1=abcdefgh"));
  assert(r.status == 200);
  assert(r.body.find("restarting") != std::string::npos);
  assert(form.saved());
  assert(store.load(stored, espnet::WIFI_STORE_MAX_NETS) == 1);
  assert(stored[0].ssid == "Home" && stored[0].pass == "abcdefgh");
  espnet::RelayConfig savedRelay;
  assert(rs_form.load(savedRelay)); // the relay configuration landed too
  assert(savedRelay.host == "192.168.1.10" && savedRelay.port == 7777);
  assert(savedRelay.key == "fleet-key-123");

  // after the save the session is done: further posts are refused, the page
  // turns into the restarting notice
  assert(form.handle("POST", "/save", saveBody("w.ssid.1=x&w.pass.1=abcdefgh")).status == 409);
  assert(form.handle("GET", "/", "").status == 200);
  assert(form.handle("GET", "/", "").body.find("restarting") != std::string::npos);

  // a second form on a fresh store: validation paths
  RamStoreBackend backend2;
  espnet::WifiStore store2;
  store2.begin(&backend2);
  RamStoreBackend rbe_form2; espnet::RelayConfigStore rs_form2; rs_form2.begin(&rbe_form2);
  espnet::SetupForm form2("espnode-setup-ddeeff", store2, stored, 0, rs_form2, espnet::RelayConfig(), kToken);;

  // a name without a password is refused, nothing stored, input brought back
  r = form2.handle("POST", "/save", saveBody("w.ssid.1=Home"));
  assert(r.status == 422);
  assert(r.body.find("type its password") != std::string::npos);
  assert(r.body.find("value=\"Home\"") != std::string::npos); // the name is preserved
  assert(backend2.blank_);
  // open network and a typed password are mutually exclusive
  assert(form2.handle("POST", "/save", saveBody("w.ssid.1=Home&w.pass.1=abcdefgh&w.open.1=1")).status == 422);
  assert(form2.handle("POST", "/save", saveBody("w.ssid.1=Home&w.pass.1=1234567")).status == 422); // 7 chars
  assert(form2.handle("POST", "/save", saveBody("w.ssid.1=Home&w.pass.1=" + std::string(64, 'x'))).status == 422);
  // a password without a name is called out
  r = form2.handle("POST", "/save", saveBody("w.pass.1=abcdefgh"));
  assert(r.status == 422);
  assert(r.body.find("network name is empty") != std::string::npos);
  // no row at all: the node would have nothing to join with
  assert(form2.handle("POST", "/save", saveBody("")).status == 422);
  assert(backend2.blank_);

  // an open network row stores an empty password
  assert(form2.handle("POST", "/save", saveBody("w.ssid.1=Cafe&w.open.1=1")).status == 200);
  assert(store2.load(stored, espnet::WIFI_STORE_MAX_NETS) == 1);
  assert(stored[0].ssid == "Cafe" && stored[0].pass.empty());

  // a third form on a store that already holds a password: a blank password
  // field keeps the stored one (editing one row does not retyping the rest)
  RamStoreBackend backend3;
  espnet::WifiStore store3;
  store3.begin(&backend3);
  espnet::WifiStoreNet existing[1];
  existing[0].ssid = "Home";
  existing[0].pass = "secret12";
  assert(store3.save(existing, 1));
  RamStoreBackend rbe_form3; espnet::RelayConfigStore rs_form3; rs_form3.begin(&rbe_form3);
  espnet::SetupForm form3("espnode-setup-112233", store3, existing, 1, rs_form3, espnet::RelayConfig(), kToken);;
  espnet::HttpReply page3 = form3.handle("GET", "/", "");
  assert(page3.body.find("value=\"Home\"") != std::string::npos);
  assert(page3.body.find("blank keeps it") != std::string::npos);
  assert(page3.body.find("secret12") == std::string::npos); // the password is never echoed
  assert(form3.handle("POST", "/save", saveBody("w.ssid.1=Home&w.ssid.2=Cafe&w.open.2=1")).status == 200);
  assert(store3.load(stored, espnet::WIFI_STORE_MAX_NETS) == 2);
  assert(stored[0].ssid == "Home" && stored[0].pass == "secret12"); // kept
  assert(stored[1].ssid == "Cafe" && stored[1].pass.empty());

  // echoed values are HTML-escaped: a stored ssid with markup cannot inject
  RamStoreBackend backend4;
  espnet::WifiStore store4;
  store4.begin(&backend4);
  espnet::WifiStoreNet evil[1];
  evil[0].ssid = "<img src=x>";
  evil[0].pass = "";
  assert(store4.save(evil, 1));
  RamStoreBackend rbe_form4; espnet::RelayConfigStore rs_form4; rs_form4.begin(&rbe_form4);
  espnet::SetupForm form4("espnode-setup-445566", store4, evil, 1, rs_form4, espnet::RelayConfig(), kToken);;
  espnet::HttpReply page4 = form4.handle("GET", "/", "");
  assert(page4.body.find("<img") == std::string::npos);
  assert(page4.body.find("&lt;img") != std::string::npos);

  // strict parsing and the caps
  RamStoreBackend rbe_form5; espnet::RelayConfigStore rs_form5; rs_form5.begin(&rbe_form5);
  espnet::SetupForm form5("espnode-setup-aabbcc", store, stored, 0, rs_form5, espnet::RelayConfig(), kToken);;
  assert(form5.handle("POST", "/save", saveBody("csrf=" + std::string(kToken) + "&csrf=" + kToken)).status == 400); // name twice
  assert(form5.handle("POST", "/save", saveBody("w.ssid.1")).status == 400); // piece without '='
  assert(form5.handle("POST", "/save", saveBody("bad name%3D1=2")).status == 400); // space in the name
  std::string big = saveBody("w.ssid.1=" + std::string(5000, 'a'));
  assert(form5.handle("POST", "/save", big).status == 413); // over the body cap
  assert(form5.handle("POST", "/save", saveBody("w.ssid.1=Home%")).status == 400); // cut %-escape
  assert(form5.handle("POST", "/save", saveBody("w.ssid.1=%00")).status == 400); // NUL byte

  // routing: captive-portal probes redirect to the form, everything else
  assert(form5.handle("GET", "/generate_204", "").status == 302);
  assert(form5.handle("GET", "/hotspot-detect.html", "").status == 302);
  {
    espnet::HttpReply probe = form5.handle("GET", "/generate_204", "");
    bool loc = false;
    for (size_t i = 0; i < probe.headers.size(); ++i) {
      if (probe.headers[i].first == "Location") {
        loc = probe.headers[i].second == "http://192.168.4.1/";
      }
    }
    assert(loc);
  }
  assert(form5.handle("GET", "/nope", "").status == 404);
  assert(form5.handle("POST", "/", "").status == 405);     // the page wants GET
  assert(form5.handle("GET", "/save", "").status == 405);  // the save wants POST
  assert(form5.handle("POST", "/save?x=1", saveBody("w.ssid.1=Cafe&w.open.1=1")).status == 200); // query ignored

  // a save on an ARMED store clears the armed flag: the SETUP_MODE request
  // is served by this very save, and the node reboots provisioned-or-not
  RamStoreBackend armedBackend;
  espnet::WifiStore armedStore;
  armedStore.begin(&armedBackend);
  espnet::WifiStoreNet seed[1];
  seed[0].ssid = "Old";
  seed[0].pass = "oldpass1";
  assert(armedStore.save(seed, 1));
  assert(armedStore.armSetup(true));
  RamStoreBackend rbe_armedForm; espnet::RelayConfigStore rs_armedForm; rs_armedForm.begin(&rbe_armedForm);
  espnet::SetupForm armedForm("espnode-setup-aabbcc", armedStore, seed, 1, rs_armedForm, espnet::RelayConfig(), kToken);;
  assert(armedForm.handle("POST", "/save", saveBody("w.ssid.1=New&w.pass.1=newpass1")).status == 200);
  assert(!armedStore.setupArmed());
  assert(armedStore.load(stored, espnet::WIFI_STORE_MAX_NETS) == 1);
  assert(stored[0].ssid == "New" && stored[0].pass == "newpass1");

  // #138: a filled row BEYOND the 30-row capacity is an explicit error, not
  // a silent drop - and 30 full rows still save cleanly
  RamStoreBackend capBackend;
  espnet::WifiStore capStore;
  capStore.begin(&capBackend);
  espnet::WifiStoreNet none[1];
  RamStoreBackend rbe_capForm; espnet::RelayConfigStore rs_capForm; rs_capForm.begin(&rbe_capForm);
  espnet::SetupForm capForm("espnode-setup-aabbcc", capStore, none, 0, rs_capForm, espnet::RelayConfig(), kToken);;
  std::string body30;
  for (int i = 1; i <= espnet::WIFI_STORE_MAX_NETS; ++i) {
    body30 += "w.ssid." + std::to_string(static_cast<long long>(i)) + "=net" +
              std::to_string(static_cast<long long>(i)) + "&w.pass." +
              std::to_string(static_cast<long long>(i)) + "=abcdefgh" + (i < espnet::WIFI_STORE_MAX_NETS ? "&" : "");
  }
  espnet::HttpReply capOk = capForm.handle("POST", "/save", saveBody(body30));
  assert(capOk.status == 200);
  assert(capStore.load(stored, espnet::WIFI_STORE_MAX_NETS) == espnet::WIFI_STORE_MAX_NETS);

  // a fresh form for the refusal (the one above is in its restarting state)
  RamStoreBackend capBackend2;
  espnet::WifiStore capStore2;
  capStore2.begin(&capBackend2);
  RamStoreBackend rbe_capForm2; espnet::RelayConfigStore rs_capForm2; rs_capForm2.begin(&rbe_capForm2);
  espnet::SetupForm capForm2("espnode-setup-aabbcc", capStore2, none, 0, rs_capForm2, espnet::RelayConfig(), kToken);;
  std::string body31 = saveBody(body30) + "&w.ssid.31=extra&w.pass.31=abcdefgh";
  espnet::HttpReply capRefused = capForm2.handle("POST", "/save", body31);
  assert(capRefused.status == 422); // the refused save re-renders the form with the errors
  assert(capRefused.body.find("up to 30 WiFi networks") != std::string::npos);
  assert(capStore2.load(stored, espnet::WIFI_STORE_MAX_NETS) == 0); // nothing was stored
}

// ── task #135: the setup session (SetupSession) ──

class FakeNet : public espnet::ISetupNet {
 public:
  bool startAccessPoint(const std::string& ssid, const std::string& password) override {
    ++startCalls_;
    lastSsid_ = ssid;
    lastPassword_ = password;
    if (failNext_ > 0) {
      --failNext_;
      return false;
    }
    up_ = true;
    return true;
  }
  void poll() override { ++polls_; }
  void stop() override { ++stops_; up_ = false; }
  bool up_ = false;
  int startCalls_ = 0, polls_ = 0, stops_ = 0, failNext_ = 0;
  std::string lastSsid_, lastPassword_;
};

class FakeLog : public espnet::ISetupLog {
 public:
  void line(const std::string& text) override { lines_.push_back(text); }
  std::vector<std::string> lines_;
};

// ── task #152: the save path's heap buffers and its serial narration ──

class VecSetupLog : public espnet::ISetupLog {
 public:
  void line(const std::string& text) override { lines.push_back(text); }
  bool has(const std::string& part) const {
    for (size_t i = 0; i < lines.size(); ++i) {
      if (lines[i].find(part) != std::string::npos) return true;
    }
    return false;
  }
  std::vector<std::string> lines;
};

static void testRegionScratchAndFullSave() {
  // the flash-region scratch lives on the heap (a 3000-byte local array
  // overflowed the loop task's stack on the first Save, #152): zeroed,
  // exactly the size asked for, a fresh buffer per object
  espnet::RegionScratch a(espnet::WIFI_STORE_REGION_MAX);
  espnet::RegionScratch b(espnet::RELAY_CONFIG_REGION_MAX);
  assert(a.ok() && b.ok());
  assert(a.size() == espnet::WIFI_STORE_REGION_MAX && b.size() == espnet::RELAY_CONFIG_REGION_MAX);
  assert(a.data() != b.data());
  for (size_t i = 0; i < a.size(); ++i) assert(a.data()[i] == 0);

  // the biggest save the form can make: 30 networks with the longest legal
  // name and password, through SetupForm::save into BOTH stores and back
  RamStoreBackend backend;
  espnet::WifiStore store;
  store.begin(&backend);
  RamStoreBackend relayBackend;
  espnet::RelayConfigStore relayStore;
  relayStore.begin(&relayBackend);
  espnet::SetupForm form("espnode-setup-152152", store, nullptr, 0, relayStore, espnet::RelayConfig(), kToken);
  std::string rows;
  for (int i = 1; i <= espnet::WIFI_STORE_MAX_NETS; ++i) {
    const std::string n = std::to_string(i);
    std::string name = "net-" + n;
    name.resize(espnet::WIFI_STORE_SSID_MAX, 'x');
    std::string pass = "pw-" + n;
    pass.resize(espnet::WIFI_STORE_PASS_MAX, 'y');
    if (i > 1) rows += "&";
    rows += "w.ssid." + n + "=" + name + "&w.pass." + n + "=" + pass;
  }
  espnet::HttpReply r = form.handle("POST", "/save", saveBody(rows));
  assert(r.status == 200 && form.saved());
  std::vector<espnet::WifiStoreNet> back(espnet::WIFI_STORE_MAX_NETS);
  assert(store.load(back.data(), espnet::WIFI_STORE_MAX_NETS) == espnet::WIFI_STORE_MAX_NETS);
  assert(back[29].ssid.size() == espnet::WIFI_STORE_SSID_MAX && back[29].pass.size() == espnet::WIFI_STORE_PASS_MAX);
  espnet::RelayConfig relayBack;
  assert(relayStore.load(relayBack) && relayBack.host == "192.168.1.10");

  // the page renders all 30 stored rows (renderForm's row tables are heap too)
  RamStoreBackend backend2;
  espnet::WifiStore store2;
  store2.begin(&backend2);
  RamStoreBackend relayBackend2;
  espnet::RelayConfigStore relayStore2;
  relayStore2.begin(&relayBackend2);
  espnet::SetupForm full("espnode-setup-152153", store2, back.data(), espnet::WIFI_STORE_MAX_NETS, relayStore2,
                         espnet::RelayConfig(), kToken);
  espnet::HttpReply page = full.handle("GET", "/", "");
  assert(page.status == 200);
  assert(page.body.find("name=\"w.ssid.30\"") != std::string::npos);
  assert(page.body.find("name=\"w.ssid.31\"") == std::string::npos);
}

static void testSetupSaveNarration() {
  RamStoreBackend backend;
  espnet::WifiStore store;
  store.begin(&backend);
  RamStoreBackend relayBackend;
  espnet::RelayConfigStore relayStore;
  relayStore.begin(&relayBackend);
  VecSetupLog log;
  espnet::SetupForm form("espnode-setup-152154", store, nullptr, 0, relayStore, espnet::RelayConfig(), kToken);

  // silent by default: no log attached, nothing breaks
  assert(form.handle("POST", "/save", "csrf=nope").status == 403);
  form.setLog(&log);

  // refusals say why
  assert(form.handle("POST", "/save", "csrf=nope").status == 403);
  assert(log.has("save: refused - the CSRF token does not match"));
  // a validation failure names the count and the problems, writes nothing
  assert(form.handle("POST", "/save", saveBody("w.ssid.1=Home")).status == 422);
  assert(log.has("save: rejected - 1 validation error(s), nothing written"));
  assert(backend.blank_);

  // a flash failure is named (and the node stays in setup) — and since #171
  // the line and the page carry the EXACT cause, so a stuck install can be
  // diagnosed from the serial monitor alone
  backend.failWrite_ = true;
  espnet::HttpReply failed = form.handle("POST", "/save", saveBody("w.ssid.1=Home&w.pass.1=hunter2hunter2"));
  assert(failed.status == 500);
  assert(log.has("save: FAILED - the WiFi list could not be written to flash (the flash write did not persist)"));
  assert(failed.body.find("Could not write the credentials to flash (the flash write did not persist)") !=
         std::string::npos);
  assert(backend.blank_); // the failed write changed nothing
  backend.failWrite_ = false;
  relayBackend.failWrite_ = true;
  failed = form.handle("POST", "/save", saveBody("w.ssid.1=Home&w.pass.1=hunter2hunter2"));
  assert(failed.status == 500);
  assert(log.has("save: FAILED - the relay configuration could not be written to flash (the flash write did not persist)"));
  assert(failed.body.find("Could not write the relay configuration to flash (the flash write did not persist)") !=
         std::string::npos);
  assert(failed.body.find("the WiFi list was saved") != std::string::npos);
  relayBackend.failWrite_ = false;
  // the partial failure STORED the WiFi list: the retry's form shows the
  // stored rows ("blank keeps it"), so the password does not have to be
  // typed again (#171 — the form used to keep its stale factory snapshot)
  espnet::HttpReply retry = form.handle("GET", "/", "");
  assert(retry.body.find("value=\"Home\"") != std::string::npos);
  assert(retry.body.find("password set - blank keeps it") != std::string::npos);
  assert(retry.body.find("hunter2hunter2") == std::string::npos); // still never echoed
  // and a retry save can leave the password box blank on the stored row:
  // the good save walks every stage in order (the stored password is kept)
  assert(form.handle("POST", "/save", saveBody("w.ssid.1=Home")).status == 200);
  assert(log.has("save: valid - 1 network(s), relay 192.168.1.10:7777"));
  assert(log.has("save: WiFi list written"));
  assert(log.has("save: both flash stores written"));
  // the session is done now: a further save is refused
  assert(form.handle("POST", "/save", saveBody("w.ssid.1=Home&w.pass.1=hunter2hunter2")).status == 409);

  // NO secret ever reaches the serial port: not the WiFi password, not the
  // relay key, not the CSRF token
  for (size_t i = 0; i < log.lines.size(); ++i) {
    assert(log.lines[i].find("hunter2hunter2") == std::string::npos);
    assert(log.lines[i].find("fleet-key-123") == std::string::npos);
    assert(log.lines[i].find(kToken) == std::string::npos);
  }
}

static void testSetupSession() {
  using espnet::SetupSessionConfig;
  using espnet::SetupStep;

  // a store + form pair the session watches; the save arrives mid-test
  RamStoreBackend backend;
  espnet::WifiStore store;
  store.begin(&backend);
  espnet::WifiStoreNet none[1];
  RamStoreBackend rbe_form; espnet::RelayConfigStore rs_form; rs_form.begin(&rbe_form);
  espnet::SetupForm form("espnode-setup-aabbcc", store, none, 0, rs_form, espnet::RelayConfig(), kToken);;
  FakeNet net;
  FakeLog log;

  SetupSessionConfig cfg;
  cfg.ssid = "espnode-setup-aabbcc";
  cfg.password = ESPNET_SETUP_AP_PASSWORD;
  cfg.graceMs = 1500;
  cfg.retryMs = 5000;
  cfg.serveCapMs = 0; // factory case: serve until saved, however long it takes

  espnet::SetupSession session(cfg, net, form, &log);

  // start: the AP comes up with the SSID and the password, no timeouts
  assert(session.step(1000) == SetupStep::Running);
  assert(net.up_ && net.startCalls_ == 1);
  assert(net.lastSsid_ == "espnode-setup-aabbcc");
  assert(net.lastPassword_ == std::string(ESPNET_SETUP_AP_PASSWORD));
  assert(session.serving());
  assert(session.step(1000 + 3600000) == SetupStep::Running); // an hour later: still serving
  bool announced = false;
  for (size_t i = 0; i < log.lines_.size(); ++i) {
    if (log.lines_[i].find("is up") != std::string::npos) announced = true;
  }
  assert(announced);
  // the WiFi password is never logged
  for (size_t i = 0; i < log.lines_.size(); ++i) {
    assert(log.lines_[i].find(std::string(ESPNET_SETUP_AP_PASSWORD)) == std::string::npos);
  }

  // the form saves through its own HTTP path: the session gives the browser
  // the reply (grace), stops the AP, then answers Restart
  assert(form.handle("POST", "/save", saveBody("w.ssid.1=Home&w.pass.1=abcdefgh")).status == 200);
  assert(session.step(3605000) == SetupStep::Running);   // inside the grace window
  assert(net.up_);                                       // still serving so the reply gets out
  assert(session.step(3605000 + 1499) == SetupStep::Running);
  assert(session.step(3605000 + 1500) == SetupStep::Restart);
  assert(!net.up_ && net.stops_ >= 1);
  session.stop(); // idempotent
  assert(net.stops_ >= 1);

  // a failed AP start is retried after retryMs, never silently given up
  RamStoreBackend backend2;
  espnet::WifiStore store2;
  store2.begin(&backend2);
  RamStoreBackend rbe_form2; espnet::RelayConfigStore rs_form2; rs_form2.begin(&rbe_form2);
  espnet::SetupForm form2("espnode-setup-ddeeff", store2, none, 0, rs_form2, espnet::RelayConfig(), kToken);;
  FakeNet net2;
  net2.failNext_ = 2;
  SetupSessionConfig cfg2 = cfg;
  espnet::SetupSession session2(cfg2, net2, form2, &log);
  assert(session2.step(0) == SetupStep::Running);
  assert(!net2.up_ && net2.startCalls_ == 1 && net2.stops_ >= 1); // cleaned up for the retry
  assert(session2.step(4000) == SetupStep::Running);  // inside retryMs: waiting
  assert(session2.step(5000) == SetupStep::Running);  // retry: fails again
  assert(session2.step(10000) == SetupStep::Running); // retry: succeeds
  assert(net2.up_ && net2.startCalls_ == 3 && session2.serving());

  // the fallback window: serveCapMs bounds the serving; its expiry answers
  // Handback and leaves the WiFi clean for the normal flow (no restart)
  RamStoreBackend backend3;
  espnet::WifiStore store3;
  store3.begin(&backend3);
  RamStoreBackend rbe_form3; espnet::RelayConfigStore rs_form3; rs_form3.begin(&rbe_form3);
  espnet::SetupForm form3("espnode-setup-112233", store3, none, 0, rs_form3, espnet::RelayConfig(), kToken);;
  FakeNet net3;
  SetupSessionConfig cfg3 = cfg;
  cfg3.serveCapMs = 60000;
  espnet::SetupSession session3(cfg3, net3, form3, &log);
  assert(session3.step(100000) == SetupStep::Running);
  assert(session3.step(100000 + 59999) == SetupStep::Running);
  assert(session3.step(100000 + 60000) == SetupStep::Handback);
  assert(!net3.up_ && net3.stops_ >= 1);
  assert(!session3.serving());
}

// ── task #135: the bounded request reader (HttpRequestReader) ──

static void testHttpRequestReader() {
  using R = espnet::HttpRequestReader;
  R reader;

  // a plain GET
  const std::string getReq = "GET / HTTP/1.1\r\nHost: 192.168.4.1\r\n\r\n";
  assert(reader.feed(getReq.data(), getReq.size()) == R::Complete);
  assert(reader.method() == "GET" && reader.target() == "/");
  assert(reader.host() == "192.168.4.1");

  // a POST body arriving in pieces (the socket rarely delivers it all at once)
  reader.reset();
  const std::string head = "POST /save HTTP/1.1\r\nHost: 192.168.4.1\r\nContent-Length: 11\r\n\r\n";
  assert(reader.feed(head.data(), 10) == R::NeedMore);
  assert(reader.feed(head.data() + 10, head.size() - 10) == R::NeedMore);
  assert(reader.feed("csrf=abcde", 10) == R::NeedMore);
  assert(reader.feed("fgh&x=1", 7) == R::Complete);
  assert(reader.method() == "POST" && reader.target() == "/save");
  assert(reader.body() == "csrf=abcdef"); // cut exactly at Content-Length
  assert(reader.body().size() == 11);

  // a POST without Content-Length: 411
  reader.reset();
  assert(reader.feed("POST /save HTTP/1.1\r\nHost: x\r\n\r\n", 32) == R::Rejected);
  assert(reader.rejectStatus() == 411);

  // an announced body over the cap: 413, refused before the body is waited for
  reader.reset();
  std::string bigHead = "POST /save HTTP/1.1\r\nContent-Length: 999999\r\n\r\n";
  assert(reader.feed(bigHead.data(), bigHead.size()) == R::Rejected);
  assert(reader.rejectStatus() == 413);

  // a request head over the cap: 431
  reader.reset();
  std::string longHead = "GET /";
  longHead += std::string(3000, 'a');
  longHead += " HTTP/1.1\r\n\r\n";
  assert(reader.feed(longHead.data(), longHead.size()) == R::Rejected);
  assert(reader.rejectStatus() == 431);

  // chunked bodies are not read: 501
  reader.reset();
  std::string chunked = "POST /save HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n";
  assert(reader.feed(chunked.data(), chunked.size()) == R::Rejected);
  assert(reader.rejectStatus() == 501);

  // two different Content-Length values: request smuggling, 400
  reader.reset();
  std::string smuggle = "POST /save HTTP/1.1\r\nContent-Length: 5\r\nContent-Length: 9\r\n\r\n";
  assert(reader.feed(smuggle.data(), smuggle.size()) == R::Rejected);
  assert(reader.rejectStatus() == 400);

  // a bare LF in the head, a control byte, an uppercase-less method, a bad
  // version, a target that is not a path: 400
  reader.reset();
  std::string bareLf = "GET / HTTP/1.1\nHost: x\r\n\r\n";
  assert(reader.feed(bareLf.data(), bareLf.size()) == R::Rejected && reader.rejectStatus() == 400);
  reader.reset();
  std::string ctrl = "GET /\x01 HTTP/1.1\r\n\r\n";
  assert(reader.feed(ctrl.data(), ctrl.size()) == R::Rejected && reader.rejectStatus() == 400);
  reader.reset();
  std::string lowerMethod = "get / HTTP/1.1\r\n\r\n";
  assert(reader.feed(lowerMethod.data(), lowerMethod.size()) == R::Rejected && reader.rejectStatus() == 400);
  reader.reset();
  std::string badVersion = "GET / HTTP/9\r\n\r\n";
  assert(reader.feed(badVersion.data(), badVersion.size()) == R::Rejected && reader.rejectStatus() == 400);
  reader.reset();
  std::string noSlash = "GET x HTTP/1.1\r\n\r\n";
  assert(reader.feed(noSlash.data(), noSlash.size()) == R::Rejected && reader.rejectStatus() == 400);

  // more headers than the cap: 431
  reader.reset();
  std::string many = "GET / HTTP/1.1\r\n";
  for (int i = 0; i < 40; ++i) many += "X-a" + std::to_string(i) + ": 1\r\n";
  many += "\r\n";
  assert(reader.feed(many.data(), many.size()) == R::Rejected && reader.rejectStatus() == 431);

  // a rejected reader stays rejected (no partial re-parses)
  assert(reader.feed("GET / HTTP/1.1\r\n\r\n", 18) == R::Rejected);
}

// ── task #135: the WHOLE sketch compiles and boots against the stubs ──
// (setup_ap.h / wifi_store_hw.h / the .ino are hardware glue the suite never
// compiled before; with the AP/DNS/EEPROM stub surface they ride the same
// desktop build, so a wiring typo can no longer wait for an arduino-cli run
// to be caught — the same class of escape the #131 v2 work closed.)
#include "../../arduino/ESP_node/ESP_node.ino"

static void testSketchCompiles() {
  setup(); // the stub boot: blank store, NO compiled list (v2) -> factory
  assert(effectiveCount == 0);
  assert(portalActive); // bootPortalDecision: nothing to join with -> the setup WiFi
  assert(std::string(DEVICE).rfind("esp-", 0) == 0);
  assert(portal.ssid().rfind("espnode-setup-", 0) == 0);
  assert(!ESP.restarted);
  // #152: the boot narrates why it started and how much stack it has left
  assert(Stream::serialOut.find("boot: reset reason") != std::string::npos);
  assert(Stream::serialOut.find("setup: portal window unbounded") != std::string::npos);

  // a few loop passes: the setup session serves (stub radio), no crash
  for (int i = 0; i < 3; ++i) loop();
  assert(portalActive);
  assert(!ESP.restarted);
}

// ── task #152: the reboot right after the setup form's save goes straight to
// joining the saved network (it used to re-open its own setup WiFi and sit
// in it), and a node that never managed to connect still gets the setup
// WiFi back in a bounded window so a wrong password stays fixable.
static void testSketchBootsIntoJoinAfterSave() {
  EEPROM = EEPROMClass();
  {  // what the setup form's save left in flash
    espnet::EepromRegionBackend wifiBe(0, espnet::WIFI_STORE_REGION_MAX);
    espnet::EepromRegionBackend relayBe(espnet::RELAY_CONFIG_BASE, espnet::RELAY_CONFIG_REGION_MAX);
    wifiBe.begin();
    relayBe.begin();
    espnet::WifiStore ws;
    ws.begin(&wifiBe);
    espnet::RelayConfigStore rs;
    rs.begin(&relayBe);
    espnet::WifiStoreNet nets[1];
    nets[0].ssid = "Home";
    nets[0].pass = "abcdefgh";
    espnet::RelayConfig cfg;
    cfg.host = "192.168.1.135";
    cfg.port = 8091;
    cfg.key = "fleet-key-123";
    assert(ws.save(nets, 1) && rs.save(cfg));
  }
  EEPROM.powerCycle();  // the restart
  // the sketch's statics as a real reboot leaves them (the previous test booted the same globals)
  portal.stop();
  portalActive = false;
  portalClosedMs = 0;
  provisionedDone = false;
  effectiveCount = 0;
  storedCount = 0;
  relayConfigured = false;
  ESP.restarted = false;
  Stream::serialOut.clear();

  setup();
  assert(effectiveCount == 1 && relayConfigured && !provisionedDone);
  assert(!portalActive);  // NORMAL flow: it joins first, the setup WiFi stays off
  assert(Stream::serialOut.find("setup saved - joining 1 stored network(s), relay 192.168.1.135:8091") != std::string::npos);
  assert(Stream::serialOut.find("opening the setup WiFi") == std::string::npos);
  assert(!ESP.restarted);

  // it keeps running the normal flow (scan/connect), the portal stays off ...
  WiFi.wifiStatus = WL_DISCONNECTED;
  for (int i = 0; i < 3; ++i) loop();
  assert(!portalActive);
  // ... until a whole retry window passes without a join: the setup WiFi
  // comes back (bounded), exactly once per window
  fakeNow += SETUP_RETRY_WINDOW_MS + 1;
  loop();
  assert(portalActive);
  assert(Stream::serialOut.find("still not provisioned - reopening the setup WiFi") != std::string::npos);

  portal.stop();
  portalActive = false;
  WiFi.wifiStatus = WL_CONNECTED;
  EEPROM = EEPROMClass();
}

// ── task #152: both flash regions survive a restart (the REAL boot sequence
// over the stub EEPROM that behaves like each core). On the ESP32 the
// EEPROM is one NVS blob and begin(size) resizes it: the sketch used to call
// begin(3000) and begin(4096), so every boot truncated the blob to 3000 and
// re-expanded it - the relay region (offset 3000) was erased, the saved
// node reported relay=0 and re-opened its setup WiFi forever.
static void testFlashRegionsSurviveRestart() {
  EEPROM = EEPROMClass();  // blank flash

  struct Boot {  // exactly what setup() does with the two backends
    espnet::EepromRegionBackend wifiBe{0, espnet::WIFI_STORE_REGION_MAX};
    espnet::EepromRegionBackend relayBe{espnet::RELAY_CONFIG_BASE, espnet::RELAY_CONFIG_REGION_MAX};
    espnet::WifiStore wifi;
    espnet::RelayConfigStore relay;
    Boot() {
      wifiBe.begin();
      wifi.begin(&wifiBe);
      relayBe.begin();
      relay.begin(&relayBe);
    }
  };

  {  // first boot (factory) + the setup form's save
    Boot b;
    espnet::WifiStoreNet nets[1];
    nets[0].ssid = "Home";
    nets[0].pass = "abcdefgh";
    espnet::RelayConfig cfg;
    cfg.host = "192.168.1.135";
    cfg.port = 8091;
    cfg.key = "fleet-key-123";
    assert(b.wifi.save(nets, 1));
    assert(b.relay.save(cfg));
  }
  for (int restart = 1; restart <= 3; ++restart) {  // restarts must not lose either region
    EEPROM.powerCycle();
    Boot b;
    espnet::WifiStoreNet back[espnet::WIFI_STORE_MAX_NETS];
    assert(b.wifi.load(back, espnet::WIFI_STORE_MAX_NETS) == 1);
    assert(back[0].ssid == "Home" && back[0].pass == "abcdefgh");
    espnet::RelayConfig cfg;
    assert(b.relay.load(cfg));
    assert(cfg.host == "192.168.1.135" && cfg.port == 8091 && cfg.key == "fleet-key-123");
  }

  // the first-connect flag write must not disturb the relay region either
  {
    EEPROM.powerCycle();
    Boot b;
    espnet::WifiStoreNet back[espnet::WIFI_STORE_MAX_NETS];
    assert(b.wifi.load(back, espnet::WIFI_STORE_MAX_NETS) == 1);
    assert(b.wifi.markProvisioned());
  }
  EEPROM.powerCycle();
  Boot b;
  espnet::RelayConfig cfg;
  assert(b.relay.load(cfg) && cfg.host == "192.168.1.135");
  espnet::WifiStoreNet back[espnet::WIFI_STORE_MAX_NETS];
  assert(b.wifi.load(back, espnet::WIFI_STORE_MAX_NETS) == 1 && b.wifi.provisioned());
  EEPROM = EEPROMClass();  // leave the global flash blank for the tests after this one
}

int main() {
  assertBoardSelection();

  testJsonReaders();
  printf("PASS json readers\n");
  testLogQueue();
  printf("PASS log queue\n");
  testMinLogFilter();
  printf("PASS min log filter\n");
  testFraming();
  printf("PASS framing\n");
  testPumpDispatch();
  printf("PASS pump dispatch\n");
  testPickStrongest();
  printf("PASS pick strongest\n");
  testWifiMulti();
  printf("PASS wifi multi\n");
  testOtaCompile();
  printf("PASS ota compile\n");
  testOtaFlashFailurePath();
  printf("PASS ota flash failure path (per-board flasher, task #165)\n");
  testWifiStore();
  printf("PASS wifi store\n");
  testWifiStoreV2();
  printf("PASS wifi store v2 flags\n");
  testRelayConfigStore();
  printf("PASS relay config store\n");
  testSetupDecisions();
  printf("PASS setup decisions\n");
  testSetupForm();
  printf("PASS setup form\n");
  testRegionScratchAndFullSave();
  printf("PASS region scratch + full 30-network save\n");
  testSetupSaveNarration();
  printf("PASS setup save narration (no secrets logged)\n");
  testSetupSession();
  printf("PASS setup session\n");
  testHttpRequestReader();
  printf("PASS http request reader\n");
  testFlashRegionsSurviveRestart();
  printf("PASS flash regions survive a restart\n");
  testSketchCompiles();
  printf("PASS sketch compiles\n");
  testSketchBootsIntoJoinAfterSave();
  printf("PASS saved node boots into the normal flow\n");
  printf("tests/cpp: all passed\n");
  return 0;
}
