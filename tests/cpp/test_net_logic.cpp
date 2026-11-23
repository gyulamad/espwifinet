// test_net_logic.cpp — compiles the node's real headers (net.h, ota.h,
// remote_log.h) against the stub Arduino environment and tests the parts
// that run identical on hardware: the flat-JSON readers, the wire framing
// (bytes identical to nodejs/esp-relay/lib/protocol.js), message dispatch
// into the hooks, and the log queue. The radio/flash paths remain
// hardware-test-only — the same split as espcamsys2's tests/cpp.

#include <cassert>
#include <cstdio>
#include <string>

#include "../stubs/WiFiClient.h" // pulls in Arduino.h + Update stub

// stub singletons the headers reference
std::string Stream::serialOut;
static EspStub espInstance;
static WiFiStub wifiInstance;
static UpdateStub updateInstance;
EspStub& ESP = espInstance;
WiFiStub& WiFi = wifiInstance;
UpdateStub Update = updateInstance;
static unsigned long fakeNow = 1000;
unsigned long millis() { return fakeNow; }
void delay(unsigned long) {}

#include "../../arduino/ESP8266_Node/remote_log.h"
#include "../../arduino/ESP8266_Node/net.h"
#include "../../arduino/ESP8266_Node/wifi.h"
#include "../../arduino/ESP8266_Node/ota.h"

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
  std::string gpioState;
  long gpioPin = -1;
};
static Captured g_cap; // NetHooks are C function pointers (as on hardware)

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

  WiFiClient client;
  espnet::RelayConnection conn;
  conn.begin("10.0.0.1", 7777, "esp-TEST", "key", &lg, hooks, &client);

  // The relay's real messages (same bytes the e2e suite sends over TCP).
  client.feed(framed("{\"type\":\"settings\",\"gpioPin\":14,\"gpioActiveHigh\":false,\"statusIntervalMs\":30000,\"minLogLevel\":\"error\"}"));
  client.feed(framed("{\"type\":\"set_gpio\",\"state\":\"on\",\"pin\":14,\"activeHigh\":false}"));
  client.feed(framed("{\"type\":\"ping\"}"));

  assert(conn.pumpMessages());
  assert(g_cap.settingsPin == 14 && g_cap.settingsActive == false && g_cap.settingsStatus == 30000);
  assert(g_cap.settingsMin == 2); // the pushed log level reaches the hook
  assert(g_cap.gpioState == "on" && g_cap.gpioPin == 14);

  // the ping must have been answered with a pong on the same connection
  const std::string reply = client.sentAndClear();
  assert(reply.find("{\"type\":\"pong\"}") != std::string::npos);
  const unsigned char* raw = reinterpret_cast<const unsigned char*>(reply.data());
  const uint32_t len = ((uint32_t)raw[0] << 24) | ((uint32_t)raw[1] << 16) | ((uint32_t)raw[2] << 8) | raw[3];
  assert(len == reply.size() - 4);

  // split delivery: one message chopped across two reads must still parse
  const std::string whole = framed("{\"type\":\"settings\",\"gpioPin\":5,\"gpioActiveHigh\":true,\"statusIntervalMs\":10000}");
  client.feed(whole.substr(0, 10));
  assert(conn.pumpMessages());
  client.feed(whole.substr(10));
  assert(conn.pumpMessages());
  assert(g_cap.settingsPin == 5 && g_cap.settingsActive == true && g_cap.settingsStatus == 10000);
  assert(g_cap.settingsMin == -1); // a settings message without minLogLevel keeps the compiled level

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

int main() {
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
  printf("tests/cpp: all passed\n");
  return 0;
}
