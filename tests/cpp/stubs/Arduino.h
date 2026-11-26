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
#include <map>

#define HIGH 1
#define LOW 0
#define OUTPUT 1
using uint8_t_arduino = unsigned char;

inline void pinMode(uint8_t, int) {}
inline void digitalWrite(uint8_t, int) {}

unsigned long millis();
void delay(unsigned long);
inline void yield() {} // the cores' cooperative yield: feeds the ESP8266 watchdog

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
  void begin(unsigned long) {}  // HardwareSerial::begin (the real core's baud call)
  void flush() {}               // HardwareSerial::flush (#152: the sketch drains the port before a restart)
  void println() { serialOut += "\n"; }
  void println(const char* s) { serialOut += s; serialOut += "\n"; }
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

extern Stream Serial;  // the real core's global serial port (the test TU defines it)

class EspStub {
 public:
  unsigned long getFreeHeap() { return 20000; }
  void restart() { restarted = true; }
  bool restarted = false;
#if defined(ESP32)
  // The ESP32 core's EspClass: the eFuse MAC instead of a chip id, and NO
  // getResetReason() / getChipId() / wdtFeed() (the reset reason lives in
  // esp_system.h's esp_reset_reason(), the loop WDT is off by default).
  // v1 stubbed the ESP8266 surface on both builds, so the desktop esp32
  // build could not catch the getResetReason() call the real esp32 compile
  // refused - keep this split honest (board.h's ESP32 branch must compile
  // against exactly what the real core offers).
  unsigned long long getEfuseMac() { return 0xA1B2C3D4E5F6ULL; }
#else // ESP8266
  String getResetReason() { return String("External System"); }
  unsigned getFreeContStack() { return 2500; } // the cont stack's high-water mark (#152)
  unsigned getChipId() { return 0x00A1B2C3; }
  void wdtFeed() {}
#endif
};
// (the test build defines the global `ESP` object itself)

enum WifiStatus { WL_DISCONNECTED = 6, WL_CONNECTED = 3 };
#define WIFI_OFF 0  // the real core's WiFiMode_t values (the setup glue, #135)
#define WIFI_STA 1
#define WIFI_AP 2
#ifndef WIFI_SCAN_RUNNING
#define WIFI_SCAN_RUNNING (-1)
#endif
#ifndef WIFI_SCAN_FAILED
#define WIFI_SCAN_FAILED (-2)
#endif

class IPAddressStub {
 public:
  IPAddressStub() {}
  explicit IPAddressStub(const std::string& s) : s_(s) {}
  String toString() const { return String(s_.c_str()); }
  // The real core's IPAddress::fromString: strict dotted quad in, false for anything else
  // (the discovery glue refuses to send to a malformed address, task #160).
  bool fromString(const char* c) {
    if (c == nullptr) return false;
    const std::string s(c);
    int part = 0;
    size_t i = 0;
    while (part < 4 && i < s.size()) {
      unsigned v = 0;
      size_t digits = 0;
      while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
        v = v * 10 + (unsigned)(s[i] - '0');
        if (++digits > 3 || v > 255) return false;
        ++i;
      }
      if (digits == 0) return false;
      if (++part < 4) {
        if (i >= s.size() || s[i] != '.') return false;
        ++i;
      }
    }
    if (part != 4 || i != s.size()) return false;
    s_ = s;
    return true;
  }
  std::string s_ = "192.168.1.42";
};
using IPAddress = IPAddressStub;  // the real core's type name

// WiFi stub: the connection surface net.h/ota.h use, plus the asynchronous
// scan surface wifi.h drives (scanNetworks(true) → poll scanComplete() →
// SSID(i)/RSSI(i) → scanDelete()). Tests steer it with setScanResults /
// finishScan / failScan and read lastBeginSsid to see what the code chose.
class WiFiStub {
 public:
  String SSID() { return connectedSsid; } // current connection (no index)
  long RSSI() { return connectedRssi; }
  String SSID(int i) { return (i >= 0 && i < scanCount) ? scanSsid[i] : String(""); }
  long RSSI(int i) { return (i >= 0 && i < scanCount) ? scanRssi[i] : 0; }
  IPAddressStub localIP() { return IPAddressStub(ownIp); }
  int status() { return wifiStatus; }
  bool mode(int) { return true; }  // the real WiFiClass::mode answers bool (the setup glue checks it)
  void begin(const char* ssid, const char*) { lastBeginSsid = ssid ? ssid : ""; }
  void disconnect(bool = false) { wifiStatus = WL_DISCONNECTED; }

  // ── DNS surface (task #160): what the discovery glue's resolve() drives ──
  // Both cores answer nonzero on success (ESP32 int 1, ESP8266 bool true);
  // the glue compares != 1. Tests steer it through the dns map / dnsFailAll.
  bool hostByName(const char* name, IPAddress& out) {
    lastHostLookup = name ? name : "";
    if (dnsFailAll) return false;
    std::map<std::string, std::string>::const_iterator it = dns.find(lastHostLookup);
    if (it == dns.end()) return false;
    out = IPAddress(it->second);
    return true;
  }
  std::map<std::string, std::string> dns;
  bool dnsFailAll = false;
  std::string lastHostLookup;

  // ── AP surface (#135): what the setup glue (setup_ap.h) drives ──
  bool softAP(const char* ssid, const char* pass, int channel = 1, int hidden = 0, int maxStations = 4) {
    (void)channel; (void)hidden; (void)maxStations;
    lastSoftApSsid = ssid ? ssid : "";
    lastSoftApPass = pass ? pass : "";
    apUp = true;
    return true;
  }
  IPAddressStub softAPIP() { return IPAddressStub(); }
  void softAPdisconnect(bool) { apUp = false; }
  int softAPgetStationNum() { return 0; }  // phones joined to the setup WiFi (#152 diagnostics)
  void persistent(bool) {}
  std::string lastSoftApSsid;
  std::string lastSoftApPass;
  bool apUp = false;

  // ── async scan emulation ──
  void scanNetworks(bool async) {
    (void)async;
    scanState = 0; // running
  }
  int scanComplete() {
    if (scanState == 2) return scanCount;        // results ready
    if (scanState == 3) return WIFI_SCAN_FAILED; // idle / refused
    return WIFI_SCAN_RUNNING;                    // 0: still scanning
  }
  void scanDelete() { scanState = 3; }

  // ── test controls ──
  void setScanResults(const char* const* ssids, const long* rssi, int n) {
    scanCount = (n < 8) ? n : 8;
    for (int i = 0; i < scanCount; i++) {
      scanSsid[i] = String(ssids[i]);
      scanRssi[i] = rssi[i];
    }
  }
  void finishScan() { scanState = 2; }
  void failScan() { scanState = 3; }

  String scanSsid[8];
  long scanRssi[8] = {0};
  int scanCount = 0;
  int scanState = 3; // idle
  std::string lastBeginSsid;
  String connectedSsid = String("TestNet");
  long connectedRssi = -58;
  int wifiStatus = WL_CONNECTED;
  std::string ownIp = "192.168.1.42"; // the address localIP() reports (the discovery glue's sweep base)
};
// (the test build defines the global `WiFi` object itself)

// min()/max() are macros on the ESP8266 core (mixed argument types compile),
// but the ESP32 core defines NO macros - its calls resolve to std::min and
// both sides must deduce the same type. The v1 stub carried the macros on
// both builds, so the desktop esp32 build accepted the mixed-type min() in
// wifi.h that the real esp32 toolchain refused. The ESP32 desktop build
// rides the real core's surface (see esp32/Arduino.h, which undefines these).
#if defined(ESP8266)
#define min(a, b) ((a) < (b) ? (a) : (b))
#define max(a, b) ((a) > (b) ? (a) : (b))
#endif
