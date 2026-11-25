// wifi_store.h — the node's flash-held WiFi credentials (task #135, v2).
// The stored list is the node's ONLY credential source: the networks the
// user picks on the setup page (setup_logic.h) live here, and config.h
// carries no WiFi list at all anymore. The store also remembers WHERE the
// node is in its life: `provisioned` (the first successful WiFi connect
// happened) and `setup-armed` (the operator asked for a setup session) —
// the setup WiFi opens exactly when the first install is not complete or
// setup is armed, never again on a mere outage. Pure C++ (no Arduino/ESP
// includes): the blob format, the validation rules and the store decisions
// are compiled and tested on a desktop (tests/cpp/test_net_logic.cpp); the
// flash itself is reached only through WifiStoreBackend, whose real
// implementation (wifi_store_hw.h, EEPROM on both cores) is
// hardware-test-only.
//
// Blob format (one region of the EEPROM space, versioned):
//   "ENW2" | flags (1 byte) | length (2 bytes, big endian) | entries
//   "ENW1" | length (2 bytes, big endian) | entries          (v1, migrated)
//   entry: ssid '\t' pass '\n'   (repeat)
//   flags bit 0: provisioned  — the node has completed a first successful
//              WiFi connect with the stored list (task #135 v2: the setup
//              WiFi only opens while the first install is NOT complete)
//   flags bit 1: setup-armed — the operator asked for a setup session
//              (the dashboard's SETUP_MODE maintenance switch); it clears
//              itself when the setup form saves
// Every entry is validated on save AND on load, so flash can never hand the
// picker a value the radio would refuse; anything damaged parses as "no
// stored networks". A v1 ("ENW1") region migrates on load: its entries were
// saved through the setup page, so they arrive provisioned and not armed.

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
// The EEPROM region the sketch reserves (wifi_store_hw.h maps the backend
// onto it): magic + flags + 2-byte length + the blob, comfortably above the
// worst case (5 * (32 + 1 + 63 + 1) = 485 bytes).
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
  void begin(WifiStoreBackend* backend) {
    backend_ = backend;
    provisioned_ = false;
    armed_ = false;
  }

  // The stored networks; answers 0 when the region is blank or damaged.
  // A v1 ("ENW1") region migrates: its entries arrive provisioned (they
  // were saved through the setup page, so the node has been connecting with
  // them) and not armed. The flags ride along for bootPortalDecision.
  int load(WifiStoreNet* out, int maxCount) {
    provisioned_ = false;
    armed_ = false;
    if (backend_ == nullptr || maxCount <= 0) return 0;
    char buf[WIFI_STORE_REGION_MAX];
    if (!backend_->read(buf, sizeof(buf))) return 0;
    size_t len = 0;
    bool prov = false, arm = false;
    if (!wifiStoreRegionBlob(buf, sizeof(buf), len, prov, arm)) return 0;
    const int count = wifiStoreParse(buf + (wifiStoreIsV2(buf) ? 7 : 6), len, out, maxCount);
    // The flags land only when the entries really parse: a damaged blob
    // reads as "no credentials" (which means the portal, not stale flags).
    if (count > 0) {
      provisioned_ = prov;
      armed_ = arm;
    }
    return count;
  }

  // Validates, serializes and persists. A save IS a provisioning event
  // coming from the setup form: the network list is replaced, the armed
  // flag clears itself (the re-entry request is served), and the
  // provisioned flag is left alone (it flips when the node first CONNECTS,
  // not when the form saves). false = nothing was written (the caller stays
  // in setup: a half-written store must never exist).
  bool save(const WifiStoreNet* nets, int count) {
    const bool ok = writeBlob(nets, count, provisioned_, false);
    if (ok) armed_ = false; // a save IS the disarm event (the setup form's save)
    return ok;
  }

  // The node joined a stored network for the first time: the first install
  // is complete. Persisted immediately, so a power cut right after cannot
  // resurrect the setup WiFi.
  bool markProvisioned() {
    provisioned_ = true;
    return writeCurrentFlags();
  }

  // The SETUP_MODE maintenance switch (the dashboard arms it; the setup
  // form's save disarms it).
  bool armSetup(bool on) {
    armed_ = on;
    return writeCurrentFlags();
  }

  bool provisioned() const { return provisioned_; }
  bool setupArmed() const { return armed_; }

 private:
  // Writes just the header + the CURRENT in-memory flags over the stored
  // entries: read the region back, replace magic/flags, write. false when
  // the region is unreadable (a flag flip never destroys the entries).
  bool writeCurrentFlags() {
    if (backend_ == nullptr) return false;
    char buf[WIFI_STORE_REGION_MAX];
    if (!backend_->read(buf, sizeof(buf))) return false;
    size_t len = 0;
    bool prov = false, arm = false;
    if (!wifiStoreRegionBlob(buf, sizeof(buf), len, prov, arm)) return false;
    if (!wifiStoreIsV2(buf)) {
      // A v1 region migrates in place: shift the entry blob one byte right
      // so it sits behind the v2 header (magic + flags + length), then
      // rewrite the header — the entries themselves are preserved.
      for (size_t i = len; i > 0; --i) buf[6 + i] = buf[5 + i];
    }
    return writeHeader(buf, len, provisioned_, armed_);
  }

  bool writeBlob(const WifiStoreNet* nets, int count, bool provisioned, bool armed) {
    if (backend_ == nullptr) return false;
    if (nets == nullptr && count != 0) return false;
    for (int i = 0; i < count; ++i) {
      if (!validStoreSsid(nets[i].ssid) || !validStorePass(nets[i].pass)) return false;
    }
    char buf[WIFI_STORE_REGION_MAX];
    const int len = wifiStoreSerialize(nets, count, buf + 7, sizeof(buf) - 7);
    if (len < 0) return false;
    return writeHeader(buf, static_cast<size_t>(len), provisioned, armed);
  }

  // Builds the v2 header ("ENW2" | flags | len BE16) in buf[0..6] and writes
  // the 7 + len bytes.
  bool writeHeader(char* buf, size_t len, bool provisioned, bool armed) {
    buf[0] = 'E';
    buf[1] = 'N';
    buf[2] = 'W';
    buf[3] = '2';
    buf[4] = static_cast<char>((provisioned ? 0x01 : 0x00) | (armed ? 0x02 : 0x00));
    buf[5] = static_cast<char>((len >> 8) & 0xff);
    buf[6] = static_cast<char>(len & 0xff);
    return backend_->write(buf, len + 7);
  }

  static bool wifiStoreIsV2(const char* buf) {
    return buf[0] == 'E' && buf[1] == 'N' && buf[2] == 'W' && buf[3] == '2';
  }

  // Accepts "ENW2" | flags | len or "ENW1" | len; answers false for
  // anything else. On success `len` is the entry blob's length, and the
  // migrated flags land in prov/arm (v1: provisioned, not armed). The entry
  // blob starts at buf + (v2 ? 7 : 6).
  static bool wifiStoreRegionBlob(const char* buf, size_t bufSize, size_t& len, bool& prov, bool& arm) {
    prov = false;
    arm = false;
    if (bufSize < 7) return false;
    if (wifiStoreIsV2(buf)) {
      const unsigned char flags = static_cast<unsigned char>(buf[4]);
      prov = (flags & 0x01) != 0;
      arm = (flags & 0x02) != 0;
      const unsigned hi = static_cast<unsigned char>(buf[5]);
      const unsigned lo = static_cast<unsigned char>(buf[6]);
      len = static_cast<size_t>((hi << 8) | lo);
      return len != 0 && 7 + len <= bufSize;
    }
    if (buf[0] == 'E' && buf[1] == 'N' && buf[2] == 'W' && buf[3] == '1') {
      const unsigned hi = static_cast<unsigned char>(buf[4]);
      const unsigned lo = static_cast<unsigned char>(buf[5]);
      len = static_cast<size_t>((hi << 8) | lo);
      prov = true;  // a v1 store was saved through the setup page: provisioned
      arm = false;
      return len != 0 && 6 + len <= bufSize;
    }
    return false;
  }

  WifiStoreBackend* backend_ = nullptr;
  bool provisioned_ = false;
  bool armed_ = false;
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
