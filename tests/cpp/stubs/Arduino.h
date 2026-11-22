// Arduino.h stub — minimal fake of the Arduino core for the desktop test
// build (tests/cpp/). Mirrors only what the sketch's headers actually use.

#pragma once

#include <string>
#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <cstdarg>
#include <algorithm>

#define HIGH 1
#define LOW 0
#define OUTPUT 1
using uint8_t_arduino = unsigned char;

inline void pinMode(uint8_t, int) {}
inline void digitalWrite(uint8_t, int) {}

unsigned long millis();
void delay(unsigned long);

class String {
  std::string s_;
 public:
  String() {}
  String(const char* c) : s_(c ? c : "") {}
  String(int v) { char b[24]; snprintf(b, sizeof(b), "%d", v); s_ = b; }
  String(long v) { char b[24]; snprintf(b, sizeof(b), "%ld", v); s_ = b; }
  String(unsigned long v) { char b[24]; snprintf(b, sizeof(b), "%lu", v); s_ = b; }
  const char* c_str() const { return s_.c_str(); }
  unsigned length() const { return (unsigned)s_.size(); }
  String substring(unsigned a, unsigned b) const {
    return String(s_.substr(a, b - a).c_str());
  }
  void remove(unsigned a, unsigned n) { if (a <= s_.size()) s_.erase(a, n); }
  void trim() {
    const char* ws = " \t\r\n";
    size_t b = s_.find_first_not_of(ws);
    size_t e = s_.find_last_not_of(ws);
    s_ = (b == std::string::npos) ? std::string() : s_.substr(b, e - b + 1);
  }
  int indexOf(char c) const {
    size_t p = s_.find(c);
    return p == std::string::npos ? -1 : (int)p;
  }
  char operator[](int i) const { return s_[i]; }
  String& operator+=(char c) { s_ += c; return *this; }
  String& operator=(const char* c) { s_ = c ? c : ""; return *this; }
  bool operator==(const char* c) const { return s_ == c; }
  friend String operator+(const String& a, const String& b) {
    return String((a.s_ + b.s_).c_str());
  }
  friend String operator+(const String& a, const char* b) {
    return String((a.s_ + (b ? b : "")).c_str());
  }
  friend String operator+(const char* a, const String& b) {
    return String(((a ? a : "") + b.s_).c_str());
  }
};

class Stream {
 public:
  virtual ~Stream() {}
  void printf(const char* fmt, ...) {
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    serialOut += buf;
  }
  static std::string serialOut;
};

class EspStub {
 public:
  unsigned long getFreeHeap() { return 20000; }
  String getResetReason() { return String("External System"); }
  unsigned getChipId() { return 0x00A1B2C3; }
  void wdtFeed() {}
  void restart() { restarted = true; }
  bool restarted = false;
};
// (the test build defines the global `ESP` object itself)

enum WifiStatus { WL_DISCONNECTED = 6, WL_CONNECTED = 3 };

class IPAddressStub {
 public:
  String toString() { return String("192.168.1.42"); }
};

class WiFiStub {
 public:
  String SSID() { return String("TestNet"); }
  long RSSI() { return -58; }
  IPAddressStub localIP() { return IPAddressStub(); }
  int status() { return wifiStatus; }
  void mode(int) {}
  void begin(const char*, const char*) {}
  int wifiStatus = WL_CONNECTED;
};
// (the test build defines the global `WiFi` object itself)

#define min(a, b) ((a) < (b) ? (a) : (b))
#define max(a, b) ((a) > (b) ? (a) : (b))
