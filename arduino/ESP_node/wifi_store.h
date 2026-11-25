// wifi_store.h — the node's flash-held WiFi credential list (task #135).
// A factory-new node has no credentials to join with, so the sketch stores
// the networks the user picks on the setup page (setup_logic.h) in flash and
// uses them on every later boot. Pure C++ (no Arduino/ESP includes): the
// blob format, the validation rules and the store decisions are compiled and
// tested on a desktop (tests/cpp/test_net_logic.cpp); the flash itself is
// reached only through WifiStoreBackend, whose real implementation
// (wifi_store_hw.h, EEPROM on both cores) is hardware-test-only.
//
// Credential precedence (decided in the sketch's setup(), tested in
// effectiveNetworksCount): a valid, non-empty stored list OVERRIDES the
// compiled-in config.h list; an empty or damaged store falls back to
// config.h. That keeps existing installs (credentials compiled in) working
// untouched and gives flashed-without-credentials nodes a way to join.
//
// Blob format (one region of the EEPROM space, versioned):
//   "ENW1" | length (2 bytes, big endian) | entries
//   entry: ssid '\t' pass '\n'   (repeat)
// Every entry is validated on save AND on load, so flash can never hand the
// picker a value the radio would refuse; anything damaged parses as "no
// stored networks" and the compiled list takes over.

#pragma once

// The ESP8266 Arduino core defines min()/max() as macros, and libstdc++'s
// own headers call std::min/std::max by those names — a standard header that
// is FIRST included after the macros exist would not even parse. The sketch
// includes this file after Arduino.h, so the macros are pushed aside for the
// standard includes and restored right after (both cores are GCC, where
// push_macro/pop_macro are supported).
#pragma push_macro("min")
#pragma push_macro("max")
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif
#include <cstddef>
#include <stdint.h>
#include <string>
#pragma pop_macro("min")
#pragma pop_macro("max")

namespace espnet {

// The store's capacity and value rules. The form offers rows up to
// WIFI_STORE_MAX_NETS; SSIDs are 1..32 bytes (the radio's own limit) and
// passwords 0..63 printable ASCII characters (0 = an open network), the
// WPA2 passphrase bounds.
static const int WIFI_STORE_MAX_NETS = 5;
static const size_t WIFI_STORE_SSID_MAX = 32;
static const size_t WIFI_STORE_PASS_MAX = 63;
static const char WIFI_STORE_MAGIC[] = "ENW1";
// The EEPROM region the sketch reserves (wifi_store_hw.h maps the backend
// onto it): magic + 2-byte length + the blob, comfortably above the worst
// case (5 * (32 + 1 + 63 + 1) = 485 bytes).
static const size_t WIFI_STORE_REGION_MAX = 512;

// One stored WiFi network. pass == "" means an open network (the node joins
// it without a password); the setup form marks such rows explicitly.
struct WifiStoreNet {
  std::string ssid;
  std::string pass;
};

inline bool isPrintableAsciiText(const std::string& s) {
  for (size_t i = 0; i < s.size(); ++i) {
    if (static_cast<unsigned char>(s[i]) < 0x20 || static_cast<unsigned char>(s[i]) == 0x7f) return false;
  }
  return true;
}

// "1..32 printable bytes". A control byte in an SSID is legal per the 802.11
// frames but unusable in practice (the phone's picker shows it garbled, and
// it would break the '\t' framing below) - the setup form refuses it.
inline bool validStoreSsid(const std::string& ssid) {
  return !ssid.empty() && ssid.size() <= WIFI_STORE_SSID_MAX && isPrintableAsciiText(ssid);
}

// "" = an open network (stored and joined without a password). Everything
// else must be a WPA2 passphrase: printable ASCII, 8..63 characters.
inline bool validStorePass(const std::string& pass) {
  if (pass.empty()) return true;
  return pass.size() >= 8 && pass.size() <= WIFI_STORE_PASS_MAX && isPrintableAsciiText(pass);
}

// entries -> blob bytes ("ENW1" | len BE16 | "ssid\tpass\n"...).
// Returns the byte count, or -1 when any entry is invalid or the result
// does not fit the region. Never writes past outMax.
inline int wifiStoreSerialize(const WifiStoreNet* nets, int count, char* out, size_t outMax);

// blob bytes -> entries. Refuses everything that is not exactly the format
// the serializer writes (wrong magic, cut-off length, a bad entry anywhere,
// trailing bytes) and answers 0 - the caller then falls back to the
// compiled-in list instead of joining with damaged data.
inline int wifiStoreParse(const char* blob, size_t blobLen, WifiStoreNet* out, int maxCount);

// ── storage backend ──
// The flash half the sketch supplies (wifi_store_hw.h on hardware, a RAM
// fake in the desktop tests). read() hands back the whole region; write()
// replaces it and must not return before the data survives a restart
// (EEPROM.commit()).
class WifiStoreBackend {
 public:
  virtual ~WifiStoreBackend() {}
  virtual bool read(char* buf, size_t max) = 0;    // false = blank/unusable region
  virtual bool write(const char* buf, size_t len) = 0; // false = the write did not persist
};

class WifiStore {
 public:
  void begin(WifiStoreBackend* backend) { backend_ = backend; }

  // The stored networks; answers 0 when the region is blank or damaged
  // (both mean "use the compiled-in list").
  int load(WifiStoreNet* out, int maxCount) {
    if (backend_ == nullptr || maxCount <= 0) return 0;
    char buf[WIFI_STORE_REGION_MAX];
    if (!backend_->read(buf, sizeof(buf))) return 0;
    size_t len = 0;
    if (!wifiStoreRegionBlob(buf, sizeof(buf), len)) return 0;
    return wifiStoreParse(buf + 6, len, out, maxCount);
  }

  // Validates, serializes and persists. false = nothing was written (the
  // caller stays in setup: a half-written store must never exist).
  bool save(const WifiStoreNet* nets, int count) {
    if (backend_ == nullptr) return false;
    if (nets == nullptr && count != 0) return false;
    for (int i = 0; i < count; ++i) {
      if (!validStoreSsid(nets[i].ssid) || !validStorePass(nets[i].pass)) return false;
    }
    char buf[WIFI_STORE_REGION_MAX];
    const int len = wifiStoreSerialize(nets, count, buf + 6, sizeof(buf) - 6);
    if (len < 0) return false;
    buf[0] = WIFI_STORE_MAGIC[0];
    buf[1] = WIFI_STORE_MAGIC[1];
    buf[2] = WIFI_STORE_MAGIC[2];
    buf[3] = WIFI_STORE_MAGIC[3];
    buf[4] = static_cast<char>((len >> 8) & 0xff);
    buf[5] = static_cast<char>(len & 0xff);
    return backend_->write(buf, static_cast<size_t>(len) + 6);
  }

 private:
  // "ENW1" + a length that fits what is left, else false. The blob starts
  // at buf + 6 and runs `len` bytes.
  static bool wifiStoreRegionBlob(const char* buf, size_t bufSize, size_t& len) {
    if (bufSize < 6) return false;
    if (buf[0] != WIFI_STORE_MAGIC[0] || buf[1] != WIFI_STORE_MAGIC[1] ||
        buf[2] != WIFI_STORE_MAGIC[2] || buf[3] != WIFI_STORE_MAGIC[3]) return false;
    const unsigned hi = static_cast<unsigned char>(buf[4]);
    const unsigned lo = static_cast<unsigned char>(buf[5]);
    len = static_cast<size_t>((hi << 8) | lo);
    return len != 0 && 6 + len <= bufSize;
  }

  WifiStoreBackend* backend_ = nullptr;
};

inline int wifiStoreSerialize(const WifiStoreNet* nets, int count, char* out, size_t outMax) {
  if (count < 0 || (nets == nullptr && count != 0)) return -1;
  std::string blob;
  for (int i = 0; i < count; ++i) {
    if (!validStoreSsid(nets[i].ssid) || !validStorePass(nets[i].pass)) return -1;
    blob += nets[i].ssid;
    blob += '\t';
    blob += nets[i].pass;
    blob += '\n';
  }
  if (blob.size() > outMax) return -1;
  for (size_t i = 0; i < blob.size(); ++i) out[i] = blob[i];
  return static_cast<int>(blob.size());
}

inline int wifiStoreParse(const char* blob, size_t blobLen, WifiStoreNet* out, int maxCount) {
  if (blob == nullptr || out == nullptr || maxCount <= 0) return 0;
  int count = 0;
  size_t pos = 0;
  while (pos < blobLen) {
    size_t tab = pos;
    while (tab < blobLen && blob[tab] != '\t') ++tab;
    if (tab >= blobLen) return 0;               // an entry without the separator
    size_t nl = tab + 1;
    while (nl < blobLen && blob[nl] != '\n') ++nl;
    if (nl >= blobLen) return 0;                // a cut-off entry
    WifiStoreNet net;
    net.ssid.assign(blob + pos, tab - pos);
    net.pass.assign(blob + tab + 1, nl - tab - 1);
    if (!validStoreSsid(net.ssid) || !validStorePass(net.pass)) return 0;
    if (count >= maxCount) return 0;            // more entries than the store holds
    out[count++] = net;
    pos = nl + 1;
  }
  return count;
}

} // namespace espnet
