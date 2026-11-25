// ESP8266WiFi.h stub — only the pieces net.h / ota.h / remote_log.h use.

#pragma once

#include "Arduino.h"

class WiFiClient {
  std::string incoming_;  // bytes the relay "sent"
  std::string outgoing_;  // bytes the node "wrote"
  bool open_ = true;
 public:
  int available() { return (int)incoming_.size(); }
  int read() {
    if (incoming_.empty()) return -1;
    unsigned char c = (unsigned char)incoming_[0];
    incoming_.erase(0, 1);
    return c;
  }
  int read(uint8_t* buf, size_t size) {  // the buffer read the setup glue uses (#135)
    size_t n = size < incoming_.size() ? size : incoming_.size();
    for (size_t i = 0; i < n; ++i) buf[i] = static_cast<uint8_t>(incoming_[i]);
    incoming_.erase(0, n);
    return static_cast<int>(n);
  }
  size_t write(const uint8_t* buf, size_t n) {
    outgoing_.append(reinterpret_cast<const char*>(buf), n);
    return n;
  }
  size_t print(const char* s) { outgoing_ += s; return strlen(s); }
  size_t print(char c) { outgoing_ += c; return 1; }
  void flush() {}
  bool connected() const { return open_; }
  void stop() { open_ = false; }
  bool connect(const char*, uint16_t) { return open_; }
  operator bool() const { return open_; }  // the real core's client converts to bool

  // test helpers
  void feed(const std::string& data) { incoming_ += data; }
  std::string sentAndClear() { std::string out; out.swap(outgoing_); return out; }
};

// WiFiServer: the tiny server surface the setup glue (#135) accepts requests
// on (the real cores' WiFiServer begins on a port and hands out clients).
class WiFiServer {
 public:
  explicit WiFiServer(uint16_t) {}
  void begin() {}
  void stop() {}
  WiFiClient available() { return WiFiClient(); }
};
