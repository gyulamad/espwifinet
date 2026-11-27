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
#include <memory>
#include <new>
#include <stdint.h>
#include <string>
#pragma pop_macro("min")
#pragma pop_macro("max")

namespace espnet {

// The store's capacity and value rules. The form offers rows up to
// WIFI_STORE_MAX_NETS; SSIDs are 1..32 bytes (the radio's own limit) and
// passwords 0..63 printable ASCII characters (0 = an open network), the
// WPA2 passphrase bounds.
//
// 30 networks is the honest cap the flash imposes: the EEPROM space is one
// 4096-byte flash sector, an entry is at most 97 bytes (32 ssid + tab +
// 63 password + newline), so 30 entries (2910) + the header fit with room
// to spare, and the REST of the sector stays free for the node's relay
// configuration region (task #140, which lands at offset 3000).
static const int WIFI_STORE_MAX_NETS = 30;
static const size_t WIFI_STORE_SSID_MAX = 32;
static const size_t WIFI_STORE_PASS_MAX = 63;
// The EEPROM region the sketch reserves for the WiFi store
// (wifi_store_hw.h maps the backend onto it): magic + flags + 2-byte
// length + the blob, above the worst case (30 * 97 = 2910 bytes).
static const size_t WIFI_STORE_REGION_MAX = 3000;

// A flash-region-sized scratch buffer on the HEAP (task #152). The store
// blobs are 3000 / 1096 bytes: as local arrays they sat on the loop task's
// stack (about 4 KB on the ESP8266, 8 KB on the ESP32) and, stacked under
// the setup form's own frames, overflowed it on the first Save - the node
// reset mid-request and the phone's page never got an answer. The desktop
// tests run on an 8 MB stack and could not see it (tests/stack_budget.sh
// now guards the frame sizes). Allocation failure is answered with ok() ==
// false, never an exception: the cores build with -fno-exceptions.
class RegionScratch {
 public:
  explicit RegionScratch(size_t size) : data_(new (std::nothrow) char[size]()), size_(size) {}
  bool ok() const { return data_ != nullptr; }
  char* data() { return data_.get(); }
  size_t size() const { return size_; }

 private:
  std::unique_ptr<char[]> data_;
  size_t size_;
};

// Why a save refused to write (task #171): the setup page's failure answer
// and the serial narration name the exact cause, so a stuck install can be
// diagnosed from the serial monitor (or even from the phone's error page)
// instead of presenting four possible causes as one. Optional out-parameter:
// the plain two-argument save() keeps working for every existing caller.
enum class StoreWriteError {
  None,        // the save succeeded
  NoBackend,   // begin(&backend) never ran
  BadInput,    // validation refused a row / the configuration before any write
  NoMemory,    // the heap scratch buffer could not be allocated
  NoSpace,     // the serialized content does not fit the store's region
  WriteFailed  // the backend's write did not persist (the flash refused)
};

// One short human-readable clause per cause, for log lines and the page.
inline const char* storeWriteErrorText(StoreWriteError why) {
  switch (why) {
    case StoreWriteError::NoBackend: return "no flash backend was started";
    case StoreWriteError::BadInput: return "a value did not pass validation";
    case StoreWriteError::NoMemory: return "the heap scratch buffer could not be allocated";
    case StoreWriteError::NoSpace: return "the content does not fit the store's region";
    case StoreWriteError::WriteFailed: return "the flash write did not persist";
    case StoreWriteError::None: break;
  }
  return "no error";
}

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
    RegionScratch scratch(WIFI_STORE_REGION_MAX);
    if (!scratch.ok()) return 0;
    char* buf = scratch.data();
    if (!backend_->read(buf, scratch.size())) return 0;
    size_t len = 0;
    bool prov = false, arm = false;
    if (!wifiStoreRegionBlob(buf, scratch.size(), len, prov, arm)) return 0;
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
  // in setup: a half-written store must never exist). `why` (optional)
  // receives the exact cause (task #171).
  bool save(const WifiStoreNet* nets, int count, StoreWriteError* why = nullptr) {
    StoreWriteError cause = StoreWriteError::None;
    const bool ok = writeBlob(nets, count, provisioned_, false, &cause);
    if (why != nullptr) *why = ok ? StoreWriteError::None : cause;  // always: a stale cause must never survive into a success
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
    RegionScratch scratch(WIFI_STORE_REGION_MAX);
    if (!scratch.ok()) return false;
    char* buf = scratch.data();
    if (!backend_->read(buf, scratch.size())) return false;
    size_t len = 0;
    bool prov = false, arm = false;
    if (!wifiStoreRegionBlob(buf, scratch.size(), len, prov, arm)) return false;
    if (!wifiStoreIsV2(buf)) {
      // A v1 region migrates in place: shift the entry blob one byte right
      // so it sits behind the v2 header (magic + flags + length), then
      // rewrite the header — the entries themselves are preserved.
      for (size_t i = len; i > 0; --i) buf[6 + i] = buf[5 + i];
    }
    return writeHeader(buf, len, provisioned_, armed_, nullptr);
  }

  bool writeBlob(const WifiStoreNet* nets, int count, bool provisioned, bool armed, StoreWriteError* why) {
    if (backend_ == nullptr) { if (why) *why = StoreWriteError::NoBackend; return false; }
    if (nets == nullptr && count != 0) { if (why) *why = StoreWriteError::BadInput; return false; }
    for (int i = 0; i < count; ++i) {
      if (!validStoreSsid(nets[i].ssid) || !validStorePass(nets[i].pass)) {
        if (why) *why = StoreWriteError::BadInput;
        return false;
      }
    }
    RegionScratch scratch(WIFI_STORE_REGION_MAX);
    if (!scratch.ok()) { if (why) *why = StoreWriteError::NoMemory; return false; }
    char* buf = scratch.data();
    const int len = wifiStoreSerialize(nets, count, buf + 7, scratch.size() - 7);
    if (len < 0) { if (why) *why = StoreWriteError::NoSpace; return false; }
    return writeHeader(buf, static_cast<size_t>(len), provisioned, armed, why);
  }

  // Builds the v2 header ("ENW2" | flags | len BE16) in buf[0..6] and writes
  // the 7 + len bytes.
  bool writeHeader(char* buf, size_t len, bool provisioned, bool armed, StoreWriteError* why) {
    buf[0] = 'E';
    buf[1] = 'N';
    buf[2] = 'W';
    buf[3] = '2';
    buf[4] = static_cast<char>((provisioned ? 0x01 : 0x00) | (armed ? 0x02 : 0x00));
    buf[5] = static_cast<char>((len >> 8) & 0xff);
    buf[6] = static_cast<char>(len & 0xff);
    if (!backend_->write(buf, len + 7)) { if (why) *why = StoreWriteError::WriteFailed; return false; }
    return true;
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

// ── The node's relay configuration (task #140) ──────────────────────────
// The relay's address and the fleet's shared secret are stored in flash
// exactly like the WiFi list: the setup page writes them, the sketch reads
// them at boot, and NOTHING has to be compiled in. That is what removes
// config.h from the end-user path entirely: a flashed binary plus one
// visit to the setup page is a complete install (no IDE, no source).
struct RelayConfig {
  std::string host;   // the relay's IP or hostname: 1..64 bytes, no spaces
  int port = 7777;    // 1..65535 (the blank field on the form means 7777)
  std::string key;    // the fleet's nodeKey: 1..64 printable bytes
};

// The relay-configuration region shares the EEPROM space with the WiFi
// store: it starts at RELAY_CONFIG_BASE, right after the 3000-byte WiFi
// region, inside the same 4096-byte sector. The hardware backend maps the
// offset (wifi_store_hw.h); the pure store only ever sees its own buffer.
// Worst case content is ~135 bytes, so 1096 bytes is plenty.
static const size_t RELAY_CONFIG_BASE = 3000;
static const size_t RELAY_CONFIG_REGION_MAX = 1096;  // 4096 - 3000

// The size the node asks the EEPROM emulation for: BOTH regions, always, from
// every backend (task #152). On the ESP32 the "EEPROM" is one NVS blob and
// EEPROM.begin(size) RESIZES it in flash: a begin() smaller than the stored
// blob truncates it for good. The WiFi backend used to begin(3000) and the
// relay backend begin(4096), so every boot cut the relay region (offset 3000)
// off the blob and re-expanded it with 0xFF - the saved relay settings were
// gone after the first restart and the node re-opened its setup WiFi forever.
static const size_t EEPROM_TOTAL_SIZE = RELAY_CONFIG_BASE + RELAY_CONFIG_REGION_MAX;

inline bool validRelayHost(const std::string& s) {
  if (s.empty() || s.size() > 64) return false;
  if (!isPrintableAsciiText(s)) return false;
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == ' ') return false;  // hostnames and IP addresses have no spaces
  }
  return true;
}

inline bool validRelayKey(const std::string& s) {
  return !s.empty() && s.size() <= 64 && isPrintableAsciiText(s);
}

// Blob format (its own region, versioned):
//   "ENRC" | set (1 byte, 1 = configured) | length (2 bytes, big endian) |
//   host '\t' port '\t' key
// A blank or damaged region reads as "not configured" — which means the
// setup WiFi opens: without a relay to report to the node has no job.
class RelayConfigStore {
 public:
  void begin(WifiStoreBackend* backend) { backend_ = backend; }

  // true + `out` when a valid configuration is stored; false when the
  // region is blank, cleared or damaged (the boot decision opens the
  // setup WiFi in every one of those cases).
  bool load(RelayConfig& out) {
    out = RelayConfig();
    if (backend_ == nullptr) return false;
    RegionScratch scratch(RELAY_CONFIG_REGION_MAX);
    if (!scratch.ok()) return false;
    char* buf = scratch.data();
    if (!backend_->read(buf, scratch.size())) return false;
    size_t len = 0;
    bool set = false;
    if (!relayRegionBlob(buf, scratch.size(), len, set)) return false;
    const std::string blob(buf + 7, len);
    const size_t t1 = blob.find('\t');
    if (t1 == std::string::npos) return false;
    const size_t t2 = blob.find('\t', t1 + 1);
    if (t2 == std::string::npos) return false;
    RelayConfig cfg;
    cfg.host = blob.substr(0, t1);
    const std::string port = blob.substr(t1 + 1, t2 - t1 - 1);
    cfg.key = blob.substr(t2 + 1);
    long p = 0;
    if (port.empty()) return false;
    for (size_t i = 0; i < port.size(); ++i) {
      if (port[i] < '0' || port[i] > '9') return false;
      p = p * 10 + (port[i] - '0');
      if (p > 65535) return false;
    }
    if (!validRelayHost(cfg.host) || !validRelayKey(cfg.key) || p < 1) return false;
    cfg.port = static_cast<int>(p);
    out = cfg;
    return set;
  }

  // Validates BEFORE anything is written (all-or-nothing, like the WiFi
  // store): a half-written relay configuration must never exist. `why`
  // (optional) receives the exact cause (task #171).
  bool save(const RelayConfig& cfg, StoreWriteError* why = nullptr) {
    if (backend_ == nullptr) { if (why) *why = StoreWriteError::NoBackend; return false; }
    if (!validRelayHost(cfg.host) || !validRelayKey(cfg.key) ||
        cfg.port < 1 || cfg.port > 65535) {
      if (why) *why = StoreWriteError::BadInput;
      return false;
    }
    std::string payload = cfg.host;
    payload += '\t';
    payload += std::to_string(static_cast<long long>(cfg.port));
    payload += '\t';
    payload += cfg.key;
    RegionScratch scratch(RELAY_CONFIG_REGION_MAX);
    if (!scratch.ok()) { if (why) *why = StoreWriteError::NoMemory; return false; }
    char* buf = scratch.data();
    if (payload.size() + 7 > scratch.size()) { if (why) *why = StoreWriteError::NoSpace; return false; }
    buf[0] = 'E';
    buf[1] = 'N';
    buf[2] = 'R';
    buf[3] = 'C';
    buf[4] = static_cast<char>(0x01);
    buf[5] = static_cast<char>((payload.size() >> 8) & 0xff);
    buf[6] = static_cast<char>(payload.size() & 0xff);
    for (size_t i = 0; i < payload.size(); ++i) buf[7 + i] = payload[i];
    if (!backend_->write(buf, payload.size() + 7)) {
      if (why) *why = StoreWriteError::WriteFailed;
      return false;
    }
    if (why) *why = StoreWriteError::None;  // always: a stale cause must never survive into a success
    return true;
  }

  // Marks the region blank (the next boot opens the setup WiFi).
  bool clear() {
    if (backend_ == nullptr) return false;
    char buf[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    return backend_->write(buf, sizeof(buf));
  }

 private:
  // "ENRC" + a length that fits what is left, else false.
  static bool relayRegionBlob(const char* buf, size_t bufSize, size_t& len, bool& set) {
    if (bufSize < 7) return false;
    if (buf[0] != 'E' || buf[1] != 'N' || buf[2] != 'R' || buf[3] != 'C') return false;
    set = static_cast<unsigned char>(buf[4]) != 0;
    const unsigned hi = static_cast<unsigned char>(buf[5]);
    const unsigned lo = static_cast<unsigned char>(buf[6]);
    len = static_cast<size_t>((hi << 8) | lo);
    return len != 0 && 7 + len <= bufSize;
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
