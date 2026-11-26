// setup_ap.h — the Arduino half of the cable-free first install (task
// #135): the node's own WiFi access point, a captive-portal DNS and a small
// HTTP server that hands every request to the pure form handler
// (SetupForm::handle in setup_logic.h). What is DECIDED (when the setup WiFi
// opens, what the form accepts, when to restart) lives in setup_logic.h /
// wifi_store.h and is desktop-tested; this file only joins it to the radio
// and to sockets. It is hardware glue: the desktop tests never compile it,
// exactly like the radio paths of wifi.h.
//
// Everything here is deliberately small and bounded: one reader per
// connection, a fixed slot count, a request timeout, and a DNS that answers
// every name so a phone's captive-portal check opens the form by itself.
//
// Design points worth knowing before changing anything:
//  * The HTTP side is a small bounded reader on WiFiServer, not the
//    WebServer library: WebServer reads a POST body of any announced size
//    into the heap before the handler runs and does not hand over the raw
//    urlencoded body, but the form handler needs both a hard size cap and
//    the exact bytes (strict decoding, duplicate-field detection).
//  * The AP is never open: a password outside 8..63 characters is refused
//    BEFORE the radio is touched, because softAP() with an empty password
//    silently starts an unprotected network.
//  * WiFi.persistent(false) while the AP is up: otherwise the setup network
//    itself is written to flash on every session. stop() puts it back to
//    the core's default (true).
//  * The reply's headers (security headers, Location, Allow) are the form's
//    decision and are sent as they are; this file only adds the framing
//    (status line, Content-Type, Content-Length, Connection: close) and
//    drops any handler header that could break it (CR/LF, a second
//    Content-Length, ...).
//  * The WiFi password is never printed. Logs carry the SSID and the URL
//    only.
//  * poll() never blocks for long and never calls delay(); loop() keeps
//    feeding the watchdog, like the rest of the sketch.
//
// Knobs (define before including to change):
//   ESPNET_SETUP_HTTP_PORT      80     captive-portal probes assume 80
//   ESPNET_SETUP_DNS_PORT       53     the phones' DNS queries
//   ESPNET_SETUP_MAX_CLIENTS    4      open HTTP connections; more are dropped
//   ESPNET_SETUP_HEAD_MAX       2048   bytes of request line + headers
//   ESPNET_SETUP_RQ_TMO_MS      10000  a request incomplete by then is dropped
//   ESPNET_SETUP_AP_CHANNEL     1      the access point's WiFi channel
//   ESPNET_SETUP_AP_STATIONS    4      phones that may be joined at once

#pragma once

#include <Arduino.h>

// (Same min/max macro push-aside as wifi_store.h/setup_logic.h: the ESP8266
// core defines the macros in Arduino.h, and these standard headers must be
// parsed before they exist — first-including them after Arduino.h fails on
// the real core, not only in the desktop stub.)
#pragma push_macro("min")
#pragma push_macro("max")
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif
#include <functional>
#include <memory>
#include <string>
#pragma pop_macro("min")
#pragma pop_macro("max")

#include "board.h"
#include "setup_logic.h"
#include "wifi_store.h"

#if defined(ESP32)
#include <DNSServer.h>
#include <esp_system.h> // esp_random()
#elif defined(ESP8266)
#include <DNSServer.h>
#include <esp8266_peri.h> // RANDOM_REG32: the hardware RNG register
#else
#error "setup_ap.h targets the supported ESP Arduino cores."
#endif

#ifndef ESPNET_SETUP_HTTP_PORT
#define ESPNET_SETUP_HTTP_PORT 80
#endif
#ifndef ESPNET_SETUP_DNS_PORT
#define ESPNET_SETUP_DNS_PORT 53
#endif
#ifndef ESPNET_SETUP_MAX_CLIENTS
#define ESPNET_SETUP_MAX_CLIENTS 4
#endif
#ifndef ESPNET_SETUP_HEAD_MAX
#define ESPNET_SETUP_HEAD_MAX 2048
#endif
#ifndef ESPNET_SETUP_RQ_TMO_MS
#define ESPNET_SETUP_RQ_TMO_MS 10000
#endif
#ifndef ESPNET_SETUP_AP_CHANNEL
#define ESPNET_SETUP_AP_CHANNEL 1
#endif
#ifndef ESPNET_SETUP_AP_STATIONS
#define ESPNET_SETUP_AP_STATIONS 4
#endif
// How long answer() keeps trying to push a reply into a socket whose send
// buffer is momentarily full (task #152): the radio can sit silent for a
// while right after the save's flash writes, and a phone ACKing slowly must
// never lose the "saved, restarting" page. yield() between attempts keeps
// the watchdog fed; the deadline bounds how long one slot may hold the loop.
#ifndef ESPNET_SETUP_REPLY_TMO_MS
#define ESPNET_SETUP_REPLY_TMO_MS 2000
#endif

#ifndef SETUP_INFO
#define SETUP_INFO(...) Serial.printf("[setup] " __VA_ARGS__), Serial.println()
#endif
#ifndef SETUP_WARN
#define SETUP_WARN(...) Serial.printf("[setup] WARN " __VA_ARGS__), Serial.println()
#endif

namespace espnet {

// The hardware RNG, per board: the CSRF token must be unguessable. Glue
// only (the desktop tests inject fixed words into csrfTokenFromRandom).
inline uint32_t espRandom() {
#if defined(ESP32)
  return esp_random();
#else
  return RANDOM_REG32;
#endif
}

// The session's log lines go to the serial port (the node is being installed
// by someone standing next to it; the relay is not reachable in this state).
class SetupSerialLog : public ISetupLog {
 public:
  void line(const std::string& text) override { Serial.printf("%s\n", text.c_str()); }
};

inline const char* setupReasonPhrase(int status) {
  switch (status) {
    case 200: return "OK";
    case 302: return "Found";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 408: return "Request Timeout";
    case 409: return "Conflict";
    case 411: return "Length Required";
    case 413: return "Payload Too Large";
    case 422: return "Unprocessable Entity";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    default: return NULL;
  }
}

// A header the handler asked for is sent only when it cannot break the
// message: a real name, no control character anywhere (CR/LF would end the
// header block and let a value inject headers or a body), and not one of the
// four the framing is built from here.
inline bool setupSafeHeaderValue(const std::string& v) {
  if (v.empty()) return false;
  for (size_t i = 0; i < v.size(); ++i) {
    if (static_cast<unsigned char>(v[i]) < 0x20 || static_cast<unsigned char>(v[i]) == 0x7f) return false;
  }
  return true;
}

inline bool setupIsFramingHeader(const std::string& name) {
  std::string n;
  for (size_t i = 0; i < name.size(); ++i) {
    n += (name[i] >= 'A' && name[i] <= 'Z') ? static_cast<char>(name[i] - 'A' + 'a') : name[i];
  }
  return n == "content-type" || n == "content-length" || n == "connection" || n == "transfer-encoding";
}

inline bool setupSendableHeader(const std::string& name, const std::string& value) {
  if (name.empty() || name.size() > 64 || value.size() > 512 || setupIsFramingHeader(name)) return false;
  for (size_t i = 0; i < name.size(); ++i) {
    unsigned char c = static_cast<unsigned char>(name[i]);
    if (c <= 0x20 || c >= 0x7f || c == ':') return false;
  }
  return setupSafeHeaderValue(value);
}

// The whole answer as bytes.
inline std::string setupFormatReply(const HttpReply& reply) {
  const int status = setupReasonPhrase(reply.status) != NULL ? reply.status : 500;
  std::string type = setupSafeHeaderValue(reply.contentType) ? reply.contentType : "text/plain";
  std::string out = "HTTP/1.1 " + std::to_string(status) + " " + setupReasonPhrase(status) + "\r\n";
  out += "Content-Type: " + type + "\r\n";
  out += "Content-Length: " + std::to_string(reply.body.size()) + "\r\n";
  out += "Connection: close\r\n";
  for (size_t i = 0; i < reply.headers.size(); ++i) {
    if (setupSendableHeader(reply.headers[i].first, reply.headers[i].second)) {
      out += reply.headers[i].first + ": " + reply.headers[i].second + "\r\n";
    }
  }
  out += "\r\n";
  out += reply.body;
  return out;
}

// What the glue itself answers when it refuses a request before the form
// sees it.
inline HttpReply setupRejection(int status) {
  HttpReply r;
  r.status = status;
  r.contentType = "text/plain";
  r.body = std::string(setupReasonPhrase(status) != NULL ? setupReasonPhrase(status) : "Error") + "\n";
  r.headers.push_back(std::make_pair("Cache-Control", "no-store"));
  r.headers.push_back(std::make_pair("X-Content-Type-Options", "nosniff"));
  return r;
}

// ── the real network ────────────────────────────────────────────────────
class ArduinoSetupNet : public ISetupNet {
 public:
  explicit ArduinoSetupNet(SetupForm& form)
      : form_(form), server_(ESPNET_SETUP_HTTP_PORT), touched_(false), running_(false),
        dnsStarted_(false), serverStarted_(false) {}
  ~ArduinoSetupNet() { stop(); }

  bool startAccessPoint(const std::string& ssid, const std::string& password) override {
    // Refuse BEFORE touching the radio: softAP() with an empty password
    // opens an unprotected network.
    if (ssid.empty() || ssid.size() > WIFI_STORE_SSID_MAX || password.size() < 8 ||
        password.size() > WIFI_STORE_PASS_MAX) {
      SETUP_WARN("access point refused: the SSID or password has an invalid length");
      return false;
    }
    stop();  // a second start begins from a clean radio
    touched_ = true;
    WiFi.persistent(false);  // do not write the setup network to flash
    if (!WiFi.mode(WIFI_AP)) return failStart("WiFi.mode(WIFI_AP) failed");
    if (!WiFi.softAP(ssid.c_str(), password.c_str(), ESPNET_SETUP_AP_CHANNEL, 0,
                     ESPNET_SETUP_AP_STATIONS)) {
      return failStart("softAP() failed");
    }
    const IPAddress ip = WiFi.softAPIP();
    dns_.setErrorReplyCode(DNSReplyCode::NoError);  // answer every name: phones then open the form by themselves
    dnsStarted_ = dns_.start(ESPNET_SETUP_DNS_PORT, "*", ip);
    if (!dnsStarted_) return failStart("the DNS server did not start");
    server_.begin();
    serverStarted_ = true;
    running_ = true;
    const std::string where = ip.toString().c_str();
    // The form redirects captive-portal probes to SETUP_AP_IP; the cores'
    // default AP address is that one.
    if (where != SETUP_AP_IP) {
      SETUP_WARN("the access point has address %s, the form redirects to %s", where.c_str(), SETUP_AP_IP);
    }
    SETUP_INFO("setup WiFi up: join it and open http://%s/", where.c_str());
    return true;
  }

  void poll() override {
    if (!running_) return;
    dns_.processNextRequest();
    acceptNew();
    for (int i = 0; i < ESPNET_SETUP_MAX_CLIENTS; ++i) {
      if (slots_[i].used) serve(slots_[i]);
    }
  }

  // Safe to call at any time and more than once; a session that never
  // started an AP is not touched. Leaves the radio as the normal flow
  // expects it (wifiMulti.begin() sets WIFI_STA from there).
  void stop() override {
    if (!touched_) return;
    for (int i = 0; i < ESPNET_SETUP_MAX_CLIENTS; ++i) release(slots_[i]);
    if (dnsStarted_) dns_.stop();
    if (serverStarted_) server_.stop();
    dnsStarted_ = serverStarted_ = running_ = touched_ = false;
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_OFF);
    WiFi.persistent(true);  // the core's default, which the sketch has been running with
    SETUP_INFO("setup WiFi down");
  }

  bool running() const { return running_; }

 private:
  struct Slot {
    bool used = false;
    uint32_t startMs = 0;
    WiFiClient client;
    HttpRequestReader reader;
  };

  bool failStart(const char* why) {
    SETUP_WARN("%s", why);
    stop();
    return false;
  }

  void acceptNew() {
    // at most one accept per slot per poll, so a flood cannot keep this
    // loop busy
    for (int n = 0; n < ESPNET_SETUP_MAX_CLIENTS; ++n) {
      WiFiClient c = server_.available();
      if (!c) return;
      Slot* free = NULL;
      for (int i = 0; i < ESPNET_SETUP_MAX_CLIENTS && !free; ++i) {
        if (!slots_[i].used) free = &slots_[i];
      }
      if (!free) {
        c.stop();  // all slots busy: drop it, the phone retries
        continue;
      }
      free->used = true;
      free->startMs = millis();
      free->client = c;
      free->reader.reset();
    }
  }

  void serve(Slot& s) {
    if (static_cast<uint32_t>(millis() - s.startMs) > ESPNET_SETUP_RQ_TMO_MS) {
      answer(s, setupRejection(408));
      return;
    }
    const int avail = s.client.available();
    if (avail <= 0) {
      if (!s.client.connected()) release(s);  // the phone left before the request was complete
      return;
    }
    uint8_t buf[1024];
    const int want = avail < static_cast<int>(sizeof(buf)) ? avail : static_cast<int>(sizeof(buf));
    const int n = s.client.read(buf, static_cast<size_t>(want));
    if (n <= 0) return;
    HttpRequestReader::Status st = s.reader.feed(reinterpret_cast<const char*>(buf), static_cast<size_t>(n));
    if (st == HttpRequestReader::NeedMore) return;
    if (st == HttpRequestReader::Rejected) {
      SETUP_WARN("request refused: %d", s.reader.rejectStatus());
      answer(s, setupRejection(s.reader.rejectStatus()));
      return;
    }
    // DNS rebinding guard: a page left open in the phone's browser could
    // otherwise POST a save with a crafted Host header. Allow the board's
    // own IP (with or without the default port); redirect everything else
    // to the board. HTTP/1.0 clients may omit Host; let them through.
    const std::string& h = s.reader.host();
    if (!h.empty() && h != SETUP_AP_IP && h != std::string(SETUP_AP_IP) + ":80") {
      HttpReply redir;
      redir.status = 302;
      redir.body = "Redirecting";
      redir.headers.push_back(std::make_pair("Location", std::string("http://") + SETUP_AP_IP + "/"));
      answer(s, redir);
      return;
    }
    answer(s, form_.handle(s.reader.method(), s.reader.target(), s.reader.body()));
  }

  void answer(Slot& s, const HttpReply& reply) {
    const std::string bytes = setupFormatReply(reply);
    size_t sent = 0;
    // A full send buffer (a phone ACKing slowly, or the radio still draining
    // the backlog the save's flash writes just caused) makes write() return
    // 0: keep trying with a yield between attempts instead of counting
    // instant failures — three back-to-back zero writes happen in
    // microseconds and would silently drop the reply, which the phone then
    // waits for forever (#152). The deadline and the connected() checks
    // bound the loop: a dead or stuck client releases the slot either way.
    const uint32_t deadline = millis() + ESPNET_SETUP_REPLY_TMO_MS;
    while (sent < bytes.size()) {
      if (!s.client.connected() || static_cast<int32_t>(millis() - deadline) >= 0) break;
      const size_t k = s.client.write(reinterpret_cast<const uint8_t*>(bytes.data()) + sent, bytes.size() - sent);
      if (k == 0) {
        yield();  // feeds the watchdog and gives the radio its send slot back
      } else {
        sent += k;
      }
    }
    release(s);
  }

  void release(Slot& s) {
    if (!s.used) return;
    s.client.stop();
    s.client = WiFiClient();
    s.reader.reset();
    s.used = false;
  }

  SetupForm& form_;
  WiFiServer server_;
  DNSServer dns_;
  Slot slots_[ESPNET_SETUP_MAX_CLIENTS];
  bool touched_, running_, dnsStarted_, serverStarted_;
};

// ── the composition the sketch drives ───────────────────────────────────
// Owns the setup pieces and nothing else: begin() decides nothing (it only
// loads the stored rows the form shows), start() opens the session and
// tick() runs it. The WHEN (factory state, fallback window) is the sketch's
// decision, made from the credential lists.
class SetupPortal {
 public:
  // The relay store (#140): the form prefills and rewrites the relay
  // configuration together with the WiFi list.
  void begin(WifiStore& store, RelayConfigStore& relayStore) {
    store_ = &store;
    relayStore_ = &relayStore;
    storedCount_ = store.load(stored_, WIFI_STORE_MAX_NETS);
    relayStore.load(relayCur_);  // false = factory: the form asks for everything
    setupSsid(chipId(), ssid_);
    stop();
  }

  // serveCapMs: 0 = serve forever (factory state), else the fallback
  // window's length in ms (the sketch's SETUP_SERVE_MAX_MS).
  void start(uint32_t serveCapMs) {
    stop();
    const std::string token = csrfTokenFromRandom(espRandom(), espRandom(), espRandom(), espRandom());
    form_.reset(new SetupForm(ssid_, *store_, stored_, storedCount_, *relayStore_, relayCur_, token));
    net_.reset(new ArduinoSetupNet(*form_));
    SetupSessionConfig cfg;
    cfg.ssid = ssid_;
    cfg.password = ESPNET_SETUP_AP_PASSWORD;
    cfg.serveCapMs = serveCapMs;
    session_.reset(new SetupSession(cfg, *net_, *form_, &serialLog_));
    active_ = true;
    SETUP_INFO("setup session: ssid '%s' (%s)", ssid_.c_str(),
               serveCapMs == 0 ? "serves until saved" : "bounded window");
  }

  SetupStep tick(uint32_t nowMs) {
    if (!active_ || !session_) return SetupStep::Handback;
    return session_->step(nowMs);
  }

  // Leaves the WiFi clean; safe to call at any time.
  void stop() {
    if (session_) session_->stop();
    session_.reset();
    net_.reset();
    form_.reset();
    active_ = false;
  }

  bool active() const { return active_; }
  const std::string& ssid() const { return ssid_; }
  int storedCount() const { return storedCount_; }

 private:
  WifiStore* store_ = nullptr;
  RelayConfigStore* relayStore_ = nullptr;
  RelayConfig relayCur_;
  WifiStoreNet stored_[WIFI_STORE_MAX_NETS];
  int storedCount_ = 0;
  std::string ssid_;
  SetupSerialLog serialLog_;
  std::unique_ptr<SetupForm> form_;
  std::unique_ptr<ArduinoSetupNet> net_;
  std::unique_ptr<SetupSession> session_;
  bool active_ = false;
};

} // namespace espnet
