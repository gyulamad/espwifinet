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
  std::string gpioState;
  long gpioPin = -1;
};
static Captured g_cap; // NetHooks are C function pointers (as on hardware)

static void hookSettings(long pin, bool active, long ms) {
  g_cap.settingsPin = pin;
  g_cap.settingsActive = active;
  g_cap.settingsStatus = ms;
}
static void hookSetGpio(const char* state, long pin, bool) {
  g_cap.gpioState = state;
  g_cap.gpioPin = pin;
}

static void testJsonReaders() {
  const char* settings = "{\"type\":\"settings\",\"gpioPin\":14,\"gpioActiveHigh\":false,\"statusIntervalMs\":30000}";
  char str[32];
  long num = 0;
  bool flag = true;

  assert(espnet::jsonGetString(settings, "type", str, sizeof(str)));
  assert(std::string(str) == "settings");
  assert(espnet::jsonGetInt(settings, "gpioPin", num) && num == 14);
  assert(espnet::jsonGetInt(settings, "statusIntervalMs", num) && num == 30000);
  assert(espnet::jsonGetBool(settings, "gpioActiveHigh", flag) && flag == false);

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
  client.feed(framed("{\"type\":\"settings\",\"gpioPin\":14,\"gpioActiveHigh\":false,\"statusIntervalMs\":30000}"));
  client.feed(framed("{\"type\":\"set_gpio\",\"state\":\"on\",\"pin\":14,\"activeHigh\":false}"));
  client.feed(framed("{\"type\":\"ping\"}"));

  assert(conn.pumpMessages());
  assert(g_cap.settingsPin == 14 && g_cap.settingsActive == false && g_cap.settingsStatus == 30000);
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

int main() {
  testJsonReaders();
  printf("PASS json readers\n");
  testLogQueue();
  printf("PASS log queue\n");
  testFraming();
  printf("PASS framing\n");
  testPumpDispatch();
  printf("PASS pump dispatch\n");
  testOtaCompile();
  printf("PASS ota compile\n");
  printf("tests/cpp: all passed\n");
  return 0;
}
