// relay_find.h — the Arduino half of "find the relay again when its address changed" (task #160):
// every minute or so it checks that the relay answers at the address stored in the flash relay
// configuration (RelayConfig.host, written by the setup page) and, when it does not, looks for it
// (a broadcast, then a sweep of the subnet) and stores the address it answered from. Every DECISION
// (what to look for, in which order, what to believe, when to look again, what to change) is in
// relay_find_logic.h and unit-tested; this file only joins it to WiFiUDP and the flash store. The
// desktop tests compile it against fakes of the core (tests/cpp/test_relay_find.cpp); the real radio
// can only be checked on a board.
//
// Like ota.h, call it from the normal flow, never while the setup portal is up (a sweep takes a few
// seconds and would kill the phone's session):
//     RelayFind find(relayConfigStore, relayCfg);
//     if (find.tick(relayCfg.host.c_str(), relayCfg.key.c_str(), relayDown)) { /* host was changed */ ESP.restart(); }
// `relayDown`: the node's connection to the relay is down (the sketch decides when). While the relay
// works at the stored address none of this runs, so a healthy system never sends a discovery packet.
//
// The port and the search timings are build-time knobs (ESPNET_DISCOVERY_PORT and the ESPNET_FIND_*
// seconds, overridable with -D flags like every other build default in the sketch — there is no
// config.h, task #140). The values reach this class through the constructor; the defaults are the
// ones the relay's example.config.js documents (discoveryPort 8083).

#pragma once

#include <Arduino.h>
#include <WiFiUdp.h>

#if defined(ESP32)
#include <esp_system.h>   // esp_random() — the documented hardware RNG (review ARD-015 pattern)
#else
#include <esp8266_peri.h> // RANDOM_REG32: the ESP8266's hardware RNG register
#endif

#include <string>

#include "relay_find_logic.h"
#include "wifi_store.h"

// Where this reports to. The sketch maps these to its remote log (so the relay's log file shows
// them once the connection is back); the default is the serial port.
#ifndef RELAY_FIND_INFO
#define RELAY_FIND_INFO(...) Serial.printf("[relay-find] " __VA_ARGS__), Serial.println()
#endif
#ifndef RELAY_FIND_WARN
#define RELAY_FIND_WARN(...) Serial.printf("[relay-find] WARN " __VA_ARGS__), Serial.println()
#endif

#ifndef ESPNET_DISCOVERY_PORT
#define ESPNET_DISCOVERY_PORT 8083  // the relay's discoveryPort (example.config.js); change both together
#endif

// The network as the locator sees it: ONE UDP socket, opened by the first probe of a search and
// closed by RelayFind::tick() when the search is over (sockets are few on an ESP board).
// `port`: the UDP port the probes go to — the relay's discoveryPort (ESPNET_DISCOVERY_PORT; the
// default is 8083, which the relay's own default matches).
class WifiFindEnv : public espnet_relayfind::IFindEnv {
 public:
  explicit WifiFindEnv(int port = ESPNET_DISCOVERY_PORT) : port_(port), open_(false) {}

  uint32_t nowMs() override { return millis(); }
  void pauseMs(uint32_t ms) override { delay(ms); }

  // The discovery nonce must be unpredictable: an attacker who can guess it
  // before the legitimate relay answers can race a fake reply (review ARD-013).
  // Arduino's `random()` is a PRNG in the API contract — it currently routes
  // through the hardware RNG on both cores, but that is an implementation
  // detail a future core version or third-party library could change. Call
  // the documented hardware RNG directly so the security property is explicit
  // (the same rule espcamsys2's relay_find.h states as review ARD-015).
  std::string newNonce() override {
    static const char digits[] = "0123456789abcdef";
    std::string s;
    for (int i = 0; i < 16; ++i) {
#if defined(ESP32)
      s += digits[esp_random() & 0x0f];
#else
      s += digits[RANDOM_REG32 & 0x0f];
#endif
    }
    return s;
  }

  bool resolve(const std::string& host, std::string& ip) override {
    uint8_t literal[4];
    if (espnet_relayfind::parseIpv4(host, literal)) {
      ip = host;
      return true;
    }
    IPAddress addr;
    // Both cores answer 1 (ESP32 int) / true (ESP8266 bool) on success.
    if (WiFi.hostByName(host.c_str(), addr) != 1) return false;
    ip = addr.toString().c_str();
    return true;
  }

  std::string ownIp() override { return WiFi.localIP().toString().c_str(); }

  bool send(const std::string& ip, const std::string& payload) override {
    IPAddress addr;
    if (!addr.fromString(ip.c_str()) || !ensureOpen()) return false;
    if (!udp_.beginPacket(addr, (uint16_t)port_)) return false;
    udp_.write(reinterpret_cast<const uint8_t*>(payload.data()), payload.size());
    return udp_.endPacket() == 1;
  }

  bool receive(uint32_t waitMs, std::string& fromIp, std::string& payload) override {
    if (!open_) return false;   // nothing was ever sent, so nothing can come back
    uint32_t start = millis();
    for (;;) {
      if (udp_.parsePacket() > 0) {
        uint8_t buf[160];   // an answer is 89 bytes; anything longer fails the check anyway
        int len = udp_.read(buf, sizeof(buf));
        if (len > 0) {
          fromIp = udp_.remoteIP().toString().c_str();
          payload.assign(reinterpret_cast<const char*>(buf), (size_t)len);
          return true;
        }
      }
      if ((uint32_t)(millis() - start) >= waitMs) return false;
      delay(2);
    }
  }

  void close() {
    if (!open_) return;
    udp_.stop();
    open_ = false;
  }

  int port() const { return port_; }   // for the log line that names the UDP port

 private:
  bool ensureOpen() {
    if (!open_) open_ = udp_.begin(0) == 1;   // port 0: any free port; the relay answers to wherever the probe came from
    return open_;
  }
  WiFiUDP udp_;
  int port_;
  bool open_;
};

// The relay host in the board's flash configuration: only RelayConfig.host changes, the port and
// the fleet key ride along untouched (RelayConfigStore::save validates and writes all-or-nothing).
class StoredRelayHost : public espnet_relayfind::IAddressStore {
 public:
  StoredRelayHost(espnet::RelayConfigStore& store, espnet::RelayConfig& cfg) : store_(store), cfg_(cfg) {}
  bool saveHost(const std::string& host) override {
    cfg_.host = host;
    return store_.save(cfg_);
  }

 private:
  espnet::RelayConfigStore& store_;
  espnet::RelayConfig& cfg_;
};

class RelayFind {
 public:
  // `port` and the four search timings come from the sketch's build defaults (ESPNET_DISCOVERY_PORT,
  // ESPNET_FIND_FIRST_S, ESPNET_FIND_OK_S, ESPNET_FIND_RETRY_MIN_S, ESPNET_FIND_RETRY_MAX_S). The
  // timings are milliseconds here, as RelayFinder takes them.
  explicit RelayFind(espnet::RelayConfigStore& store, espnet::RelayConfig& cfg, int port = ESPNET_DISCOVERY_PORT,
                     uint32_t firstMs = 10UL * 1000UL, uint32_t okMs = 60UL * 1000UL,
                     uint32_t retryMinMs = 15UL * 1000UL, uint32_t retryMaxMs = 300UL * 1000UL)
      : env_(port),
        address_(store, cfg),
        finder_(env_, address_, firstMs, okMs, retryMinMs, retryMaxMs) {}

  // true: the relay answered from another address and the stored relay host now holds it — restart the
  // board so every task starts with it. Does nothing unless a check is due; takes a few seconds when a
  // search is needed, so call it from the normal flow only (never while the setup portal is up).
  bool tick(const std::string& host, const std::string& apiKey, bool relayDown) {
    using espnet_relayfind::RelayFinder;
    RelayFinder::Result r = finder_.tick(relayDown && WiFi.status() == WL_CONNECTED, host, apiKey);
    if (r == RelayFinder::Idle) return false;
    env_.close();
    if (r == RelayFinder::NotFound && (finder_.failures() == 1 || finder_.failures() % 12 == 0)) {   // not every time: the log keeps the first and then one in twelve
      RELAY_FIND_WARN("relay not found (try %u): not at %s or anywhere nearby - is it off, or is UDP %d blocked on it?",
                      (unsigned)finder_.failures(), host.c_str(), env_.port());
    } else if (r == RelayFinder::Moved) {
      RELAY_FIND_INFO("the relay is at %s now (it was %s) - stored, restarting to use it",
                      finder_.found().c_str(), host.c_str());
    }
    return r == RelayFinder::Moved;
  }

 private:
  WifiFindEnv env_;
  StoredRelayHost address_;
  espnet_relayfind::RelayFinder finder_;
};
