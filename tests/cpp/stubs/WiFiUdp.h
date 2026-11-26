// WiFiUdp.h stub — the datagram surface the discovery glue (relay_find.h,
// task #160) drives on both cores. The test plays the relay (and the rest of
// the network) through fakeUdp(): its hook fires for every packet the glue
// sends and its inbox holds the answers the glue will read back, so the
// desktop build can walk a full search (probe → answer → store → restart)
// without a radio.

#pragma once

// The ESP8266 core defines min()/max() as macros (see wifi_store.h): the
// standard headers below must be parsed without them, so the same
// push_macro/pop_macro dance guards them.
#pragma push_macro("min")
#pragma push_macro("max")
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif
#include <deque>
#include <functional>
#include <string>
#include <utility>
#include <vector>
#pragma pop_macro("min")
#pragma pop_macro("max")

#include "Arduino.h"

// Everything the glue did with "the radio", steerable by the tests.
struct FakeUdpState {
  int opens = 0;
  int closes = 0;
  bool open = false;
  std::vector<std::string> sent;  // "<ip>:<port> <payload>" for every packet
  std::deque<std::pair<std::string, std::string> > inbox;  // answers waiting to be read
  // The "network": called with every sent packet; a test hook that plays relay answers into inbox.
  std::function<void(const std::string& ip, int port, const std::string& payload)> hook;
};

inline FakeUdpState& fakeUdp() {
  static FakeUdpState s;
  return s;
}

class WiFiUDP {
 public:
  int begin(uint16_t) {
    fakeUdp().open = true;
    fakeUdp().opens += 1;
    return 1;
  }
  void stop() {
    fakeUdp().open = false;
    fakeUdp().closes += 1;
  }

  int beginPacket(const IPAddress& ip, uint16_t port) {
    pendingIp_ = ip.toString().c_str();
    pendingPort_ = port;
    pending_.clear();
    return 1;
  }
  size_t write(const uint8_t* buf, size_t n) {
    pending_.append(reinterpret_cast<const char*>(buf), n);
    return n;
  }
  int endPacket() {
    fakeUdp().sent.push_back(pendingIp_ + ":" + std::to_string(pendingPort_) + " " + pending_);
    if (fakeUdp().hook) fakeUdp().hook(pendingIp_, pendingPort_, pending_);
    return 1;
  }

  int parsePacket() { return fakeUdp().inbox.empty() ? 0 : static_cast<int>(fakeUdp().inbox.front().second.size()); }
  int read(uint8_t* buf, size_t n) {
    if (fakeUdp().inbox.empty()) return 0;
    const std::pair<std::string, std::string> entry = fakeUdp().inbox.front();
    fakeUdp().inbox.pop_front();
    const size_t take = entry.second.size() < n ? entry.second.size() : n;
    for (size_t i = 0; i < take; ++i) buf[i] = static_cast<uint8_t>(entry.second[i]);
    lastFrom_ = entry.first;
    return static_cast<int>(take);
  }
  IPAddress remoteIP() { return IPAddress(lastFrom_.c_str()); }

  // test helpers
  static void reset() { fakeUdp() = FakeUdpState(); }

 private:
  std::string pendingIp_;
  int pendingPort_ = 0;
  std::string pending_;
  std::string lastFrom_;
};
