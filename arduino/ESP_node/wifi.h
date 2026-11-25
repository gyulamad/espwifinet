// wifi.h — multi-network WiFi management for the node (task #36), the
// WiFiMulti pattern:
// instead of one compiled-in SSID the node knows a LIST of networks and
// always (re)connects to the strongest one it can see. That is what makes
// moving a node between floors, rooms or extenders a matter of listing the
// networks once in config.h — no reflashing per location.
//
// The policy, deliberately conservative so a healthy link is never churned:
//   - every (re)connect scans and picks the strongest KNOWN network,
//   - an online node re-evaluates at most every WIFI_ROAM_CHECK_MS and only
//     acts when its signal DEGRADES below WIFI_WEAK_RSSI_DBM, and only hands
//     over when another known network beats the current one by at least
//     WIFI_ROAM_MARGIN_DB (so two shadowing APs cannot make it flap),
//   - scanning is asynchronous: loop() keeps running between polls, and the
//     relay connection (which rides the CURRENT wifi link) is not disturbed
//     by the scan itself.
//
// The choice logic (pickStrongestNetwork) is pure and desktop-tested in
// tests/cpp/test_net_logic.cpp; the scan/connect driving needs a radio and
// is hardware-test-only.

#pragma once

#include <Arduino.h>
#include "board.h"
#include "remote_log.h"

// ── tunables (a sketch can #define them before including wifi.h) ──
#ifndef WIFI_RETRY_MIN_MS
#define WIFI_RETRY_MIN_MS 5000   // first re-scan delay after a loss/failure
#endif
#ifndef WIFI_RETRY_MAX_MS
#define WIFI_RETRY_MAX_MS 60000  // backoff cap when nothing is visible
#endif
#ifndef WIFI_SCAN_TIMEOUT_MS
#define WIFI_SCAN_TIMEOUT_MS 12000
#endif
#ifndef WIFI_CONNECT_TIMEOUT_MS
#define WIFI_CONNECT_TIMEOUT_MS 15000
#endif
#ifndef WIFI_WEAK_RSSI_DBM
#define WIFI_WEAK_RSSI_DBM (-75) // below this a roam re-evaluation may run
#endif
#ifndef WIFI_ROAM_MARGIN_DB
#define WIFI_ROAM_MARGIN_DB 8    // a candidate must beat the current AP by this
#endif
#ifndef WIFI_ROAM_CHECK_MS
#define WIFI_ROAM_CHECK_MS 30000 // roam re-evaluations are rate-limited to this
#endif
#ifndef WIFI_SCAN_RESULTS_MAX
#define WIFI_SCAN_RESULTS_MAX 20 // scan entries considered (SSID copy buffer)
#endif

namespace espnet {

// One known WiFi network (config.h). An empty ssid marks the slot unused.
struct WifiNetwork {
  const char* ssid;
  const char* pass;
};

// Pure decision: the index into `known` of the STRONGEST network present in
// the scan; -1 when none of the known networks are visible. Unused slots
// (empty ssid) never match. First match wins ties so the choice is stable
// when two APs report the same RSSI.
inline int pickStrongestNetwork(const String* scanSsids, const long* scanRssi,
                                int scanCount, const WifiNetwork* known, int knownCount) {
  int bestKnown = -1;
  long bestRssi = -128; // dBm: more (closer to 0) is stronger
  for (int s = 0; s < scanCount; s++) {
    for (int k = 0; k < knownCount; k++) {
      if (known[k].ssid == nullptr || known[k].ssid[0] == '\0') continue; // unused slot
      if (strcmp(scanSsids[s].c_str(), known[k].ssid) != 0) continue;
      if (bestKnown == -1 || scanRssi[s] > bestRssi) {
        bestKnown = k;
        bestRssi = scanRssi[s];
      }
    }
  }
  return bestKnown;
}

// Drives the scan → pick → connect cycle (and the weak-signal roam). All
// decisions above run on the desktop tests; this class additionally owns
// the radio calls, which the stubs emulate.
class WifiMulti {
 public:
  void begin(const WifiNetwork* networks, int count, RemoteLog* log) {
    nets_ = networks;
    netCount_ = count;
    log_ = log;
    phase_ = Phase::Disconnected;
    currentNet_ = -1;
    scanForRoam_ = false;
    lastAttemptMs_ = 0;
    scanStartedMs_ = 0;
    connectStartedMs_ = 0;
    lastRoamCheckMs_ = 0;
    backoffMs_ = WIFI_RETRY_MIN_MS;
    WiFi.mode(WIFI_STA);
  }

  bool connected() const { return phase_ == Phase::Online; }
  int currentNetwork() const { return currentNet_; }
  const char* phaseName() const {
    switch (phase_) {
      case Phase::Online: return "online";
      case Phase::Scanning: return "scanning";
      case Phase::Connecting: return "connecting";
      default: return "disconnected";
    }
  }

  // Called from loop() every pass; never blocks for long.
  void ensure() {
    switch (phase_) {
      case Phase::Online: tickOnline(); break;
      case Phase::Scanning: tickScanning(); break;
      case Phase::Connecting: tickConnecting(); break;
      default: tickDisconnected(); break;
    }
  }

 private:
  enum class Phase { Disconnected, Scanning, Connecting, Online };

  void say(LogLevel level, const char* fmt, ...) {
    if (log_ == nullptr) return;
    char buf[LOG_MSG_MAX];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    log_->logf(level, "%s", buf);
  }

  void startScan(bool forRoam) {
    scanForRoam_ = forRoam;
    phase_ = Phase::Scanning;
    scanStartedMs_ = millis();
    WiFi.scanNetworks(true);
    say(LOG_INFO, "wifi: scanning (%s)", forRoam ? "weak signal" : "pick strongest");
  }

  void tickDisconnected() {
    const uint32_t now = millis();
    if (now - lastAttemptMs_ < backoffMs_) return;
    lastAttemptMs_ = now;
    startScan(false);
  }

  void tickScanning() {
    const int result = WiFi.scanComplete();
    if (result == WIFI_SCAN_RUNNING) {
      if (millis() - scanStartedMs_ > WIFI_SCAN_TIMEOUT_MS) {
        WiFi.scanDelete();
        scheduleRetry("scan timeout");
      }
      return;
    }
    if (result == WIFI_SCAN_FAILED) {
      scheduleRetry("scan failed"); // radio refused (or idle) — retry later
      return;
    }

    // results are in (result >= 0): copy the entries the picker may see
    String ssids[WIFI_SCAN_RESULTS_MAX];
    long rssi[WIFI_SCAN_RESULTS_MAX];
    const int n = (result < WIFI_SCAN_RESULTS_MAX) ? result : WIFI_SCAN_RESULTS_MAX;
    for (int i = 0; i < n; i++) {
      ssids[i] = WiFi.SSID(i);
      rssi[i] = WiFi.RSSI(i);
    }
    const int best = pickStrongestNetwork(ssids, rssi, n, nets_, netCount_);
    const long bestRssi = (best >= 0) ? strongestSeenRssi(ssids, rssi, n, nets_[best].ssid) : 0;
    WiFi.scanDelete();

    if (best < 0) {
      // a roam that finds nothing (or nothing stronger) keeps the CURRENT
      // link — only a fresh connect scan may go back to Disconnected
      if (scanForRoam_) {
        say(LOG_INFO, "wifi: nothing stronger visible — keeping %s", WiFi.SSID().c_str());
        stayOnline();
      } else {
        scheduleRetry("no known network visible");
      }
      return;
    }

    if (scanForRoam_) {
      if (best == currentNet_ || WiFi.RSSI() + WIFI_ROAM_MARGIN_DB > bestRssi) {
        stayOnline(); // the current AP is still acceptable — no churn
        return;
      }
      connectTo(best, bestRssi); // hand-over: clearly stronger AP elsewhere
      return;
    }
    connectTo(best, bestRssi);
  }

  void connectTo(int idx, long rssi) {
    currentNet_ = idx;
    phase_ = Phase::Connecting;
    connectStartedMs_ = millis();
    WiFi.begin(nets_[idx].ssid, nets_[idx].pass);
    say(LOG_INFO, "wifi: connecting to %s (%ld dBm)", nets_[idx].ssid, rssi);
  }

  void tickConnecting() {
    if (WiFi.status() == WL_CONNECTED) {
      phase_ = Phase::Online;
      currentNet_ = indexOfSsid(WiFi.SSID().c_str());
      backoffMs_ = WIFI_RETRY_MIN_MS;
      lastRoamCheckMs_ = millis();
      say(LOG_INFO, "wifi: connected to %s (%ld dBm)", WiFi.SSID().c_str(), WiFi.RSSI());
      return;
    }
    if (millis() - connectStartedMs_ > WIFI_CONNECT_TIMEOUT_MS) {
      WiFi.disconnect();
      scheduleRetry("connect timeout");
    }
  }

  void tickOnline() {
    // a dropped link (extender reboot, AP restart) goes back to reconnecting
    if (WiFi.status() != WL_CONNECTED) {
      say(LOG_WARN, "wifi: connection lost");
      phase_ = Phase::Disconnected;
      currentNet_ = -1;
      lastAttemptMs_ = 0; // rescan right away
      backoffMs_ = WIFI_RETRY_MIN_MS;
      return;
    }
    const uint32_t now = millis();
    if (now - lastRoamCheckMs_ < WIFI_ROAM_CHECK_MS) return;
    lastRoamCheckMs_ = now;
    if (WiFi.RSSI() >= WIFI_WEAK_RSSI_DBM) return; // healthy link — no churn
    startScan(true);
  }

  void stayOnline() {
    phase_ = Phase::Online;
    lastRoamCheckMs_ = millis();
  }

  void scheduleRetry(const char* why) {
    phase_ = Phase::Disconnected;
    lastAttemptMs_ = millis();
    say(LOG_WARN, "wifi: %s, retry in %lu ms", why, (unsigned long)backoffMs_);
    // (uint32_t) on the cap: the ESP8266 core defines min() as a macro and
    // takes mixed types, but the ESP32 core has no macro - this resolves to
    // std::min and both sides must deduce the same type (the real esp32
    // compile caught the int literal against backoffMs_'s uint32_t; #131 v2).
    backoffMs_ = min(backoffMs_ * 2, (uint32_t)WIFI_RETRY_MAX_MS);
  }

  int indexOfSsid(const char* ssid) const {
    for (int k = 0; k < netCount_; k++) {
      if (nets_[k].ssid != nullptr && strcmp(nets_[k].ssid, ssid) == 0) return k;
    }
    return -1;
  }

  static long strongestSeenRssi(const String* scanSsids, const long* scanRssi,
                                int scanCount, const char* ssid) {
    long best = -128;
    for (int i = 0; i < scanCount; i++) {
      if (strcmp(scanSsids[i].c_str(), ssid) == 0 && scanRssi[i] > best) best = scanRssi[i];
    }
    return best;
  }

  const WifiNetwork* nets_ = nullptr;
  int netCount_ = 0;
  RemoteLog* log_ = nullptr;
  Phase phase_ = Phase::Disconnected;
  int currentNet_ = -1;
  bool scanForRoam_ = false;
  uint32_t lastAttemptMs_ = 0;
  uint32_t scanStartedMs_ = 0;
  uint32_t connectStartedMs_ = 0;
  uint32_t lastRoamCheckMs_ = 0;
  uint32_t backoffMs_ = WIFI_RETRY_MIN_MS;
};

} // namespace espnet
