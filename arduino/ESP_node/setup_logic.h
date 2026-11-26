// setup_logic.h — the pure decisions behind the cable-free first install
// (task #135): WHEN the node opens its own setup WiFi, what that WiFi is
// called, what the setup page accepts, and when the node restarts. Plain
// C++ (no Arduino/ESP includes): the HTTP server glue (setup_ap.h) only
// hands over "method, target, body" and sends back the HttpReply, so every
// decision here is compiled and tested on a desktop
// (tests/cpp/test_net_logic.cpp), like wifi_store.h.
//
// The store (wifi_store.h) holds the WiFi list AND the two life-cycle flags:
// `provisioned` (the first successful WiFi connect happened) and
// `setup-armed` (the operator asked for a setup session via the dashboard's
// SETUP_MODE maintenance switch). The setup WiFi exists exactly while the
// first install is NOT complete (factory state, or credentials saved but
// never connected with) or setup is armed — never again on a mere outage
// (v2: a provisioned node in a WiFi outage just retries, it does not open a
// captive portal anyone nearby could join).
//
// Rules this file must get right, because the setup page can write the
// node's credentials:
//   - nothing is stored unless the WHOLE form is valid (all-or-nothing);
//   - a per-session CSRF token guards every POST, compared in constant
//     time; a session without a usable token refuses every POST;
//   - everything echoed into the page is HTML-escaped; WiFi passwords are
//     never put into the page at all — only "set" / "not set";
//   - the request is parsed strictly and its size is capped;
//   - passwords are never logged, echoed or sent back.

#pragma once

// (Same min/max macro push-aside as wifi_store.h: the ESP8266 core defines
// the macros, and these standard headers must parse before they exist.)
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
#include <utility>
#include <vector>
#pragma pop_macro("min")
#pragma pop_macro("max")

#include "wifi_store.h"

#ifndef ESPNET_SETUP_AP_PASSWORD
// The setup WiFi's WPA2 password, overridable at build time with
// -DESPNET_SETUP_AP_PASSWORD (there is no config.h to edit — task #140). It
// is in the repository, so it is public knowledge — the setup
// WiFi only exists while the node cannot join anything anyway, and the page
// behind it writes credentials, so the WPA2 gate is there to keep drive-by
// neighbours out, not to be a secret.
#define ESPNET_SETUP_AP_PASSWORD "espnode1234"
#endif

namespace espnet {

// ── Constants ───────────────────────────────────────────────────────────

// The node's own access-point address: where the form lives. The captive
// DNS (setup_ap.h) answers every name with it, so a phone that joined the
// setup WiFi opens the form by itself.
static const char* const SETUP_AP_IP = "192.168.4.1";

// "espnode-setup-<last 3 chip-id bytes, lowercase hex>", e.g.
// espnode-setup-d4e5f6. Derived from chipId() (board.h), which both cores
// take from the unique per-board MAC — no radio call needed, and two boards
// never open the same name. Empty/short ids are refused so a broken chip id
// can never produce an empty or odd SSID.
inline bool setupSsid(uint32_t chipId, std::string& out) {
  static const char digits[] = "0123456789abcdef";
  if (chipId == 0) return false;
  out = "espnode-setup-";
  for (int shift = 16; shift >= 0; shift -= 8) {
    const unsigned b = (chipId >> shift) & 0xff;
    out += digits[b >> 4];
    out += digits[b & 0x0f];
  }
  return out.size() <= WIFI_STORE_SSID_MAX;
}

// 128 bits of per-session randomness from the glue (setup_ap.h) as four
// words -> the form's CSRF token: 32 lowercase hex characters.
inline std::string csrfTokenFromRandom(uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3) {
  static const char digits[] = "0123456789abcdef";
  std::string t;
  const uint32_t words[4] = {r0, r1, r2, r3};
  for (int w = 0; w < 4; ++w) {
    uint32_t r = words[w];
    for (int i = 0; i < 8; ++i) {
      t += digits[r & 0x0f];
      r >>= 4;
    }
  }
  return t;
}

// ── Text helpers ────────────────────────────────────────────────────────

inline int hexDigitValue(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// application/x-www-form-urlencoded text -> plain text. Strict: a '%' not
// followed by two hex digits, a raw control character, or a decoded NUL
// (%00) is an error (a NUL would cut the value short wherever it meets
// C-string code), never silently "fixed". Messages never contain the text.
inline bool urlDecode(const std::string& in, std::string& out, std::string& err) {
  out.clear();
  for (size_t i = 0; i < in.size(); ++i) {
    char c = in[i];
    if (c == '+') { out += ' '; continue; }
    if (c == '%') {
      int hi = i + 1 < in.size() ? hexDigitValue(in[i + 1]) : -1;
      int lo = i + 2 < in.size() ? hexDigitValue(in[i + 2]) : -1;
      if (hi < 0 || lo < 0) { err = "a %-escape is cut off or not hexadecimal"; out.clear(); return false; }
      char decoded = static_cast<char>(hi * 16 + lo);
      if (decoded == '\0') { err = "the request contains a NUL byte"; out.clear(); return false; }
      out += decoded;
      i += 2;
      continue;
    }
    if (static_cast<unsigned char>(c) < 0x20 || static_cast<unsigned char>(c) == 0x7f) {
      err = "the request contains a raw control character"; out.clear(); return false;
    }
    out += c;
  }
  return true;
}

struct FormField {
  std::string name;
  std::string value;
};

inline bool isFieldNameChar(char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
         c == '_' || c == '.' || c == '-';
}

// "a=1&b=2" -> fields. Refuses (with a message that never repeats the
// content): a body over WIFI_SETUP_MAX_BODY, more than WIFI_SETUP_MAX_FIELDS
// fields, a piece without '=', a name that is empty, too long or not
// [A-Za-z0-9_.-], a name given twice (which of the two counts is ambiguous,
// and an ambiguity is how requests get smuggled), and anything urlDecode
// refuses. Empty pieces ("a=1&&b=2", a trailing '&') are skipped.
static const size_t WIFI_SETUP_MAX_BODY = 4096;
// #138: 30 rows x 3 fields + the csrf token = 91; the cap stays tight (a
// handful over what the biggest legitimate form can send) but is no longer
// 32, which would refuse a full 30-row save.
static const size_t WIFI_SETUP_MAX_FIELDS = 96;
static const size_t WIFI_SETUP_MAX_FIELD_NAME = 40;
static const size_t WIFI_SETUP_MIN_TOKEN = 16;  // shorter = "no token": every POST refused
static const size_t WIFI_SETUP_MIN_ROWS = 1;    // one row by default; "Add another network" adds more (#138)

inline bool parseUrlEncoded(const std::string& body, std::vector<FormField>& out, std::string& err) {
  out.clear();
  if (body.size() > WIFI_SETUP_MAX_BODY) { err = "the request is too large"; return false; }
  size_t pos = 0;
  while (pos <= body.size()) {
    size_t amp = body.find('&', pos);
    if (amp == std::string::npos) amp = body.size();
    std::string piece = body.substr(pos, amp - pos);
    pos = amp + 1;
    if (piece.empty()) continue;

    size_t eq = piece.find('=');
    if (eq == std::string::npos) { err = "a form field has no '='"; out.clear(); return false; }
    FormField f;
    std::string e;
    if (!urlDecode(piece.substr(0, eq), f.name, e) || !urlDecode(piece.substr(eq + 1), f.value, e)) {
      err = e;
      out.clear();
      return false;
    }
    bool nameOk = !f.name.empty() && f.name.size() <= WIFI_SETUP_MAX_FIELD_NAME;
    for (size_t k = 0; k < f.name.size(); ++k) if (!isFieldNameChar(f.name[k])) nameOk = false;
    if (!nameOk) { err = "a form field has an invalid name"; out.clear(); return false; }
    for (size_t k = 0; k < out.size(); ++k) {
      if (out[k].name == f.name) { err = "a form field is given twice"; out.clear(); return false; }
    }
    if (out.size() >= WIFI_SETUP_MAX_FIELDS) { err = "the request has too many fields"; out.clear(); return false; }
    out.push_back(f);
  }
  return true;
}

inline const std::string* findField(const std::vector<FormField>& fields, const std::string& name) {
  for (size_t i = 0; i < fields.size(); ++i) if (fields[i].name == name) return &fields[i].value;
  return nullptr;
}

// For text placed into HTML (element text or a quoted attribute). Control
// characters can't occur in a validated value but flash can hold anything,
// so they become spaces.
inline std::string htmlEscape(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (size_t i = 0; i < s.size(); ++i) {
    char c = s[i];
    switch (c) {
      case '&':  out += "&amp;";  break;
      case '<':  out += "&lt;";   break;
      case '>':  out += "&gt;";   break;
      case '"':  out += "&quot;"; break;
      case '\'': out += "&#39;";  break;
      default:
        out += (static_cast<unsigned char>(c) < 0x20 || static_cast<unsigned char>(c) == 0x7f) ? ' ' : c;
    }
  }
  return out;
}

// Same result whatever the first difference is, so response timing says
// nothing about how many leading characters of the token were right.
inline bool constantTimeEquals(const std::string& a, const std::string& b) {
  size_t n = a.size() > b.size() ? a.size() : b.size();
  unsigned char diff = static_cast<unsigned char>(a.size() != b.size());
  for (size_t i = 0; i < n; ++i) {
    unsigned char x = i < a.size() ? static_cast<unsigned char>(a[i]) : 0;
    unsigned char y = i < b.size() ? static_cast<unsigned char>(b[i]) : 0;
    diff = static_cast<unsigned char>(diff | (x ^ y));
  }
  return diff == 0;
}

// ── Replies ─────────────────────────────────────────────────────────────

// What the HTTP server sends back. The headers are decided here, where they
// are tested, not in the hardware glue.
struct HttpReply {
  int status;
  std::string contentType;
  std::string body;
  std::vector<std::pair<std::string, std::string> > headers;
  HttpReply() : status(500), contentType("text/plain; charset=utf-8") {}
};

inline const char* setupPageStyle() {
  return "body{font-family:system-ui,sans-serif;margin:0;background:#f3f4f6;color:#111}"
         "main{max-width:36rem;margin:0 auto;padding:1rem}"
         "h1{font-size:1.3rem}.row,fieldset,.box{background:#fff;border:1px solid #d1d5db;border-radius:.5rem;padding:.6rem .8rem;margin:.6rem 0}"
         "label{display:block;font-size:.85rem;margin-bottom:.25rem}"
         "input[type=text],input[type=password]{width:100%;box-sizing:border-box;font-size:1rem;padding:.45rem;margin:.15rem 0}"
         ".net{margin-bottom:.5rem}.net label{display:inline;font-size:.85rem}"
         ".err{background:#fef2f2;border-color:#b91c1c}.note{font-size:.85rem;color:#374151}"
         "button{font-size:1rem;padding:.6rem 1rem;margin:.3rem 0;border-radius:.4rem;border:1px solid #1f2937;background:#1f2937;color:#fff;width:100%}button.rm,#addrow{width:auto;padding:.3rem .8rem;margin:.2rem 0}";
}

inline void addSecurityHeaders(HttpReply& r, bool html) {
  r.headers.push_back(std::make_pair("Cache-Control", "no-store"));
  r.headers.push_back(std::make_pair("X-Content-Type-Options", "nosniff"));
  r.headers.push_back(std::make_pair("Referrer-Policy", "no-referrer"));
  if (html) {
    // No inline script and no third-party anything; the one inline
    // stylesheet plus the one same-origin script (/setup.js, the form's
    // Add/Remove rows helper) are all that is allowed to run.
    r.headers.push_back(std::make_pair("Content-Security-Policy",
        "default-src 'none'; style-src 'unsafe-inline'; script-src 'self'; form-action 'self'; frame-ancestors 'none'; base-uri 'none'"));
    r.headers.push_back(std::make_pair("X-Frame-Options", "DENY"));
  }
}

// The form's Add/Remove rows helper (#138), served as /setup.js from the
// node itself (same origin, the CSP's script-src 'self'). Vanilla JS, no
// dependencies, no network calls: it clones the first row for "Add", a
// Remove button drops its own row, and afterwards every row is RENUMBERED
// so the POST stays sequential (w.ssid.1..N) - the server-side parser also
// tolerates gaps, but sequential keeps the error messages readable. The
// hard cap mirrors WIFI_STORE_MAX_NETS: the node's flash sector is the
// physical limit.
inline const char* setupFormJavaScript() {
  return R"js((function(){
  var fieldset = document.querySelector('fieldset');
  var addWrap = document.getElementById('addwrap');
  var addBtn = document.getElementById('addrow');
  function renumber() {
    var rows = fieldset.querySelectorAll('.net');
    for (var i = 0; i < rows.length; i++) {
      var n = i + 1;
      var ss = rows[i].querySelector('input[type=text]');
      var pw = rows[i].querySelector('input[type=password]');
      var op = rows[i].querySelector('input[type=checkbox]');
      if (ss) ss.name = 'w.ssid.' + n;
      if (pw) pw.name = 'w.pass.' + n;
      if (op) op.name = 'w.open.' + n;
    }
  }
  addBtn.addEventListener('click', function () {
    var rows = fieldset.querySelectorAll('.net');
    if (rows.length >= 30) return; // the flash sector's cap; the server refuses more anyway
    var fresh = rows[0].cloneNode(true);
    var ss = fresh.querySelector('input[type=text]');
    var pw = fresh.querySelector('input[type=password]');
    var op = fresh.querySelector('input[type=checkbox]');
    if (ss) ss.value = '';
    if (pw) pw.value = '';
    if (op) op.checked = false;
    fieldset.insertBefore(fresh, addWrap);
    renumber();
    if (ss) ss.focus();
  });
  fieldset.addEventListener('click', function (ev) {
    if (!ev.target || !ev.target.classList || !ev.target.classList.contains('rm')) return;
    var rows = fieldset.querySelectorAll('.net');
    if (rows.length <= 1) return; // keep at least one row
    var row = ev.target.parentNode;
    if (row) row.parentNode.removeChild(row);
    renumber();
  });
  renumber();
})();)js";
}

// The /setup.js reply: the script above with the no-store/nosniff pair.
inline HttpReply setupJavaScriptReply() {
  HttpReply r;
  r.status = 200;
  r.contentType = "text/javascript; charset=utf-8";
  r.body = setupFormJavaScript();
  r.headers.push_back(std::make_pair("Cache-Control", "no-store"));
  r.headers.push_back(std::make_pair("X-Content-Type-Options", "nosniff"));
  return r;
}

inline HttpReply htmlPage(int status, const std::string& title, const std::string& bodyHtml) {
  HttpReply r;
  r.status = status;
  r.contentType = "text/html; charset=utf-8";
  r.body = "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">"
           "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\"><title>" + htmlEscape(title) +
           "</title><style>" + setupPageStyle() + "</style></head><body><main>" + bodyHtml + "</main></body></html>";
  addSecurityHeaders(r, true);
  return r;
}

inline HttpReply textReply(int status, const std::string& text) {
  HttpReply r;
  r.status = status;
  r.body = text + "\n";
  addSecurityHeaders(r, false);
  return r;
}

// Addresses that phones and laptops request to find out whether they are
// behind a captive portal. They are sent to the form (a redirect is what
// tells the operating system "sign in here").
inline bool isCaptivePortalProbe(const std::string& path) {
  static const char* const probes[] = {
      "/generate_204", "/gen_204",                                 // Android / Chrome
      "/hotspot-detect.html", "/library/test/success.html",        // Apple
      "/connecttest.txt", "/ncsi.txt", "/redirect", "/fwlink",     // Windows
      "/canonical.html", "/success.txt",                           // Firefox
      "/check_network_status.txt",                                 // Linux NetworkManager variants
  };
  for (size_t i = 0; i < sizeof(probes) / sizeof(probes[0]); ++i) if (path == probes[i]) return true;
  return false;
}

// ── The form's store rows ───────────────────────────────────────────────

inline std::string rowField(const char* part, int row) {
  return std::string("w.") + part + "." + std::to_string(static_cast<long long>(row));
}

// The submitted rows -> a validated network list. A blank name drops the
// row; "open network" and a typed password are mutually exclusive; a blank
// password keeps the stored password of the network with the same name (so
// editing one row does not mean retyping every password); a brand-new
// network needs one of the two. `stored` is the store's current list (the
// keep-password source); the caller passes it already loaded.
inline void buildStoreRows(const std::vector<FormField>& fields, const WifiStoreNet* stored, int storedCount,
                           WifiStoreNet* out, int& outCount, std::vector<std::string>& errors) {
  bool anyRow = false;
  for (int i = 1; i <= WIFI_STORE_MAX_NETS; ++i) {
    if (findField(fields, rowField("ssid", i)) || findField(fields, rowField("pass", i)) ||
        findField(fields, rowField("open", i))) anyRow = true;
  }
  if (!anyRow) {
    errors.push_back("Fill in at least one WiFi network: the node has nothing to join with");
    outCount = 0;
    return;
  }
  outCount = 0;
  for (int i = 1; i <= WIFI_STORE_MAX_NETS; ++i) {
    const std::string* ssid = findField(fields, rowField("ssid", i));
    const std::string* pass = findField(fields, rowField("pass", i));
    const std::string* open = findField(fields, rowField("open", i));
    std::string name = ssid ? *ssid : "";
    std::string word = pass ? *pass : "";
    bool isOpen = open != nullptr && *open == "1";
    std::string where = "WiFi network " + std::to_string(static_cast<long long>(i));

    if (name.empty()) {
      if (!word.empty()) errors.push_back(where + ": a password is typed but the network name is empty");
      continue;
    }
    WifiStoreNet net;
    net.ssid = name;
    if (isOpen && !word.empty()) {
      errors.push_back(where + ": tick \"open network\" or type a password, not both");
      continue;
    }
    if (isOpen) {
      if (outCount < WIFI_STORE_MAX_NETS) out[outCount++] = net;
      continue;
    }
    if (!word.empty()) {
      net.pass = word;
      if (outCount < WIFI_STORE_MAX_NETS) out[outCount++] = net;
      continue;
    }
    bool kept = false;
    for (int s = 0; s < storedCount && !kept; ++s) {
      if (stored[s].ssid == name && !stored[s].pass.empty()) {
        net.pass = stored[s].pass;
        kept = true;
      }
    }
    if (!kept) {
      errors.push_back(where + ": type its password, or tick \"open network\"");
      continue;
    }
    if (outCount < WIFI_STORE_MAX_NETS) out[outCount++] = net;
  }
  if (errors.empty() && outCount == 0) {
    errors.push_back("Fill in at least one WiFi network: the node has nothing to join with");
  }
  // A filled row beyond the store's capacity must never be SILENTLY dropped
  // (that would lose a credential the user typed) - say so instead. The
  // main loop only walks 1..MAX, so a non-empty name at an index past the
  // capacity is the only way to detect a hand-crafted or out-of-sync form.
  bool overflow = false;
  for (int i = WIFI_STORE_MAX_NETS + 1; i <= WIFI_STORE_MAX_NETS + 64 && !overflow; ++i) {
    const std::string* ssid = findField(fields, rowField("ssid", i));
    if (ssid != nullptr && !ssid->empty()) overflow = true;
  }
  if (overflow) {
    errors.push_back("The node's flash holds up to 30 WiFi networks - remove a row and save again");
  }
}

// The submitted relay fields -> a validated relay configuration (#140).
// The key follows the password rule: never echoed, a blank field keeps the
// stored one, a brand-new node needs it typed. A blank port means 7777.
// Returns false when `errors` gained entries; `out` is the best candidate
// either way (so a re-render can bring the typed host/port back).
inline bool buildRelayConfig(const std::vector<FormField>& fields, const RelayConfig& current,
                             RelayConfig& out, std::vector<std::string>& errors) {
  const std::string* host = findField(fields, "r.host");
  const std::string* port = findField(fields, "r.port");
  const std::string* key = findField(fields, "r.key");
  RelayConfig cfg;
  cfg.host = host ? *host : "";
  cfg.port = current.port;
  cfg.key = key && !key->empty() ? *key : current.key;
  bool ok = true;
  if (!validRelayHost(cfg.host)) {
    errors.push_back("Relay address: type the Pi's IP or hostname (1..64 characters, no spaces)");
    ok = false;
  }
  if (port != nullptr && !port->empty()) {
    long p = 0;
    bool digits = true;
    for (size_t i = 0; i < port->size(); ++i) {
      if ((*port)[i] < '0' || (*port)[i] > '9') { digits = false; break; }
      p = p * 10 + ((*port)[i] - '0');
      if (p > 65535) break;
    }
    if (!digits || p < 1 || p > 65535) {
      errors.push_back("Relay port: 1..65535 (blank means 7777)");
      ok = false;
    } else {
      cfg.port = static_cast<int>(p);
    }
  }
  if (!validRelayKey(cfg.key)) {
    errors.push_back(std::string("Relay key: type the relay's nodeKey") +
                     (current.key.empty() ? " (1..64 characters)" : " again, or leave the box blank to keep the stored one"));
    ok = false;
  }
  out = cfg;
  return ok;
}

// ── Rendering ───────────────────────────────────────────────────────────

// One .net row of the setup form: name + password + "open network" + the
// Remove button (#138). The names carry the 1-based row index; the browser
// script renumbers the rows when it adds or removes one, so the POST stays
// sequential. The SSID box mirrors the radio's own 32-byte limit; the
// password box deliberately has no maxlength (the "blank keeps the stored
// one" placeholder must never silently cut a longer stored password).
inline std::string netRowHtml(int index, const std::string& ssid, bool hasPassword, bool open) {
  const std::string n = std::to_string(static_cast<long long>(index));
  return "<div class=\"net\"><input type=\"text\" name=\"" + rowField("ssid", index) + "\" maxlength=\"32\" value=\"" +
         htmlEscape(ssid) + "\" placeholder=\"network name " + n +
         "\" autocapitalize=\"off\" autocomplete=\"off\" spellcheck=\"false\">"
         "<input type=\"password\" name=\"" + rowField("pass", index) + "\" autocomplete=\"new-password\" placeholder=\"" +
         std::string(hasPassword ? "password set - blank keeps it" : "password") + "\">"
         "<label><input type=\"checkbox\" name=\"" + rowField("open", index) + "\" value=\"1\"" +
         (open ? " checked" : "") + "> open network</label>"
         "<button type=\"button\" class=\"rm\">Remove</button></div>";
}

// The whole form. `submitted` / `errors` are empty on the first visit; after
// a rejected save they bring the user's (non-secret) input back so nothing
// has to be typed again. Passwords are never put into the page: a stored one
// is kept when the box is blank.
inline std::string renderForm(const std::string& title, const WifiStoreNet* stored, int storedCount,
                              const std::string& csrfToken, const std::vector<FormField>& submitted,
                              const std::vector<std::string>& errors,
                              const RelayConfig& relayPrefill, bool relayHasKey,
                              const std::string& relayHostBack, const std::string& relayPortBack) {
  // which rows to bring back: what the user submitted, else what is stored
  WifiStoreNet rows[WIFI_STORE_MAX_NETS];
  bool openRows[WIFI_STORE_MAX_NETS] = {false};
  bool hasPassword[WIFI_STORE_MAX_NETS] = {false};
  int rowCount = 0;
  size_t submittedRows = 0;
  for (int i = 1; i <= WIFI_STORE_MAX_NETS; ++i) {
    if (findField(submitted, rowField("ssid", i)) || findField(submitted, rowField("open", i))) submittedRows = static_cast<size_t>(i);
  }
  if (submittedRows > 0) {
    for (int i = 1; i <= static_cast<int>(submittedRows) && rowCount < WIFI_STORE_MAX_NETS; ++i) {
      const std::string* ssid = findField(submitted, rowField("ssid", i));
      const std::string* open = findField(submitted, rowField("open", i));
      rows[rowCount].ssid = ssid ? *ssid : std::string();
      rows[rowCount].pass.clear();          // a typed password is never brought back
      openRows[rowCount] = open != nullptr && *open == "1";
      hasPassword[rowCount] = false;
      ++rowCount;
    }
  } else {
    for (int i = 0; i < storedCount && rowCount < WIFI_STORE_MAX_NETS; ++i) {
      rows[rowCount] = stored[i];
      openRows[rowCount] = stored[i].pass.empty();
      hasPassword[rowCount] = !stored[i].pass.empty();
      ++rowCount;
    }
  }
  int shown = rowCount + 1;
  if (shown < static_cast<int>(WIFI_SETUP_MIN_ROWS)) shown = static_cast<int>(WIFI_SETUP_MIN_ROWS);
  if (shown > WIFI_STORE_MAX_NETS) shown = WIFI_STORE_MAX_NETS;

  std::string h = "<h1>Set up " + htmlEscape(title) + "</h1>";
  if (!errors.empty()) {
    h += "<div class=\"box err\" role=\"alert\"><b>Nothing was saved.</b><ul>";
    for (size_t i = 0; i < errors.size(); ++i) h += "<li>" + htmlEscape(errors[i]) + "</li>";
    h += "</ul><p class=\"note\">Passwords are never sent back to this page: type again any you entered.</p></div>";
  }
  h += "<p class=\"note\">Choose the WiFi network(s) this node should join - it always connects to the "
       "strongest of them. Leave the password box blank on a row whose network is already stored: it keeps "
       "that password. Saving writes the list to the node's flash and restarts it.</p>";
  h += "<form method=\"post\" action=\"/save\" autocomplete=\"off\"><input type=\"hidden\" name=\"csrf\" value=\"" +
       htmlEscape(csrfToken) + "\">";
  h += "<fieldset><legend>WiFi networks</legend>";
  for (int i = 0; i < shown; ++i) {
    const bool have = i < rowCount;
    h += netRowHtml(i + 1, have ? rows[i].ssid : std::string(),
                    have && hasPassword[i], have && openRows[i]);
  }
  // #138: rows are added and removed in the browser (/setup.js). Without
  // JavaScript the page still offers two extra blank rows, so a plain
  // browser can save up to three networks in one pass.
  h += "<noscript>";
  for (int i = shown; i < shown + 2 && i < WIFI_STORE_MAX_NETS; ++i) {
    h += netRowHtml(i + 1, std::string(), false, false);
  }
  h += "</noscript>";
  h += "<div id=\"addwrap\"><button type=\"button\" id=\"addrow\">Add another network</button></div>";
  h += "</fieldset>";
  // #140: the relay section — the node cannot do its job without it, so it
  // is part of the same all-or-nothing save. The key is never echoed; a
  // blank field keeps the stored one. The host/port come back typed after
  // a rejected save, like the WiFi names do.
  h += "<fieldset><legend>Relay (the Pi this node reports to)</legend>";
  const std::string hostShown = relayHostBack.empty() ? relayPrefill.host : relayHostBack;
  const std::string portShown = relayPortBack.empty()
      ? (relayPrefill.port == 7777 ? std::string() : std::to_string(static_cast<long long>(relayPrefill.port)))
      : relayPortBack;
  h += "<input type=\"text\" name=\"r.host\" maxlength=\"64\" value=\"" + htmlEscape(hostShown) +
       "\" placeholder=\"relay IP or hostname\" autocapitalize=\"off\" autocomplete=\"off\" spellcheck=\"false\">"
       "<input type=\"text\" name=\"r.port\" maxlength=\"5\" inputmode=\"numeric\" value=\"" + htmlEscape(portShown) +
       "\" placeholder=\"port (7777 if left blank)\">"
       "<input type=\"password\" name=\"r.key\" autocomplete=\"new-password\" placeholder=\"" +
       std::string(relayHasKey ? "key set - blank keeps it" : "the relay's nodeKey") + "\">";
  h += "</fieldset>";
  h += "<button type=\"submit\">Save and restart</button></form>";
  h += "<script src=\"/setup.js\"></script>";
  return h;
}

// ── The request handler ─────────────────────────────────────────────────

class SetupForm {
 public:
  // `csrfToken`: an unguessable string for this session (>= WIFI_SETUP_MIN_TOKEN
  // characters, from the hardware random generator, setup_ap.h). The form
  // writes through `store` (wifi_store.h) and shows `stored` as its current
  // rows; both come from the same WifiStore, so a save is what the next
  // render would show.
  SetupForm(const std::string& title, WifiStore& store, const WifiStoreNet* stored, int storedCount,
            RelayConfigStore& relayStore, const RelayConfig& relayCurrent,
            const std::string& csrfToken)
      : title_(title), store_(store), relayStore_(relayStore), relayCur_(relayCurrent),
        token_(csrfToken), saved_(false) {
    for (int i = 0; i < storedCount && i < WIFI_STORE_MAX_NETS; ++i) stored_[i] = stored[i];
    storedCount_ = storedCount < WIFI_STORE_MAX_NETS ? storedCount : WIFI_STORE_MAX_NETS;
  }

  // method: "GET" / "POST" (case-sensitive, as on the wire); target: the
  // request target (path and optional ?query, which is ignored); body: the
  // request body, already limited by the server.
  HttpReply handle(const std::string& method, const std::string& target, const std::string& body) {
    std::string path = target.substr(0, target.find('?'));
    if (path == "/setup.js") return method == "GET" ? setupJavaScriptReply() : notAllowed("GET");
    if (isCaptivePortalProbe(path)) return method == "GET" ? redirectToForm() : notAllowed("GET");
    if (path == "/") return method == "GET" ? formPage() : notAllowed("GET");
    if (path == "/save") return method == "POST" ? save(body) : notAllowed("POST");
    return textReply(404, "Not found");
  }

  bool saved() const { return saved_; }  // the store was written: the session restarts the node

 private:
  HttpReply notAllowed(const char* allow) {
    HttpReply r = textReply(405, "Method not allowed");
    r.headers.push_back(std::make_pair("Allow", allow));
    return r;
  }

  HttpReply redirectToForm() {
    HttpReply r = textReply(302, "Redirecting to the setup page");
    r.headers.push_back(std::make_pair("Location", std::string("http://") + SETUP_AP_IP + "/"));
    return r;
  }

  HttpReply restartingPage(const std::string& what) {
    return htmlPage(200, "Restarting", "<h1>" + htmlEscape(what) + "</h1><p>The node is restarting now and this setup WiFi " +
                        "will disappear in a few seconds. It then joins the network you saved. You can close this page.</p>");
  }

  HttpReply formPage() {
    if (saved()) return restartingPage("Saved");
    return htmlPage(200, "Setup", renderForm(title_, stored_, storedCount_, token_,
                                             std::vector<FormField>(), std::vector<std::string>(),
                                             relayCur_, !relayCur_.key.empty(), std::string(), std::string()));
  }

  // The checks every POST starts with. true: `fields` is the parsed form and
  // the token was right. false: `fail` is the answer to send, and nothing
  // has been looked at beyond the token.
  bool acceptPost(const std::string& body, std::vector<FormField>& fields, HttpReply& fail) {
    if (saved()) { fail = textReply(409, "Already saved, the node is restarting"); return false; }
    if (body.size() > WIFI_SETUP_MAX_BODY) { fail = textReply(413, "The request is too large"); return false; }
    std::string err;
    if (!parseUrlEncoded(body, fields, err)) { fail = textReply(400, "Bad request: " + err); return false; }
    const std::string* sent = findField(fields, "csrf");
    if (token_.size() < WIFI_SETUP_MIN_TOKEN || sent == nullptr || !constantTimeEquals(*sent, token_)) {
      fail = textReply(403, "Forbidden: open the setup page again and resubmit it");
      return false;
    }
    return true;
  }

  HttpReply rejected(const std::vector<FormField>& fields, const std::vector<std::string>& errors) {
    const std::string* hostBack = findField(fields, "r.host");
    const std::string* portBack = findField(fields, "r.port");
    return htmlPage(422, "Setup", renderForm(title_, stored_, storedCount_, token_, fields, errors,
                                             relayCur_, !relayCur_.key.empty(),
                                             hostBack ? *hostBack : std::string(),
                                             portBack ? *portBack : std::string()));
  }

  HttpReply save(const std::string& body) {
    std::vector<FormField> fields;
    HttpReply fail;
    if (!acceptPost(body, fields, fail)) return fail;

    WifiStoreNet nets[WIFI_STORE_MAX_NETS];
    int count = 0;
    RelayConfig relay;
    std::vector<std::string> errors;
    buildStoreRows(fields, stored_, storedCount_, nets, count, errors);
    for (int i = 0; i < count; ++i) {
      if (!validStoreSsid(nets[i].ssid)) { errors.push_back("WiFi network: the name is not usable"); break; }
      if (!validStorePass(nets[i].pass)) { errors.push_back("WiFi network: the password is not usable"); break; }
    }
    // #140: the relay fields are part of the same save — everything is
    // validated BEFORE anything is written, so a rejected save changes
    // neither the WiFi list nor the relay configuration.
    const bool relayOk = buildRelayConfig(fields, relayCur_, relay, errors);
    if (!errors.empty()) return rejected(fields, errors);

    if (!store_.save(nets, count)) {
      return textReply(500, "Could not write the credentials to flash; nothing was changed - try again");
    }
    if (!relayStore_.save(relay)) {
      // The WiFi list is in; the relay configuration refused to persist.
      // Stay in setup: the next save rewrites both (the WiFi rows the form
      // brings back are now the stored ones, and their passwords are kept
      // by the blank-field rule).
      return textReply(500, "Could not write the relay configuration to flash - the WiFi list was saved; try again");
    }
    saved_ = true;
    return restartingPage("Saved - the node will join your WiFi and report to your relay");
  }

  std::string title_;
  WifiStore& store_;
  RelayConfigStore& relayStore_;
  RelayConfig relayCur_;
  WifiStoreNet stored_[WIFI_STORE_MAX_NETS];
  int storedCount_ = 0;
  std::string token_;
  bool saved_;
};

// ── The setup session ───────────────────────────────────────────────────

// The hardware half (setup_ap.h); a fake in the tests.
struct ISetupNet {
  virtual ~ISetupNet() {}
  // true = the access point is up and the servers are listening.
  virtual bool startAccessPoint(const std::string& ssid, const std::string& password) = 0;
  virtual void poll() = 0;  // serve DNS + HTTP; never blocks for long
  virtual void stop() = 0;  // access point and servers off, WiFi left clean for the normal flow
};

// Serial log lines (optional). The password is never passed to this.
struct ISetupLog {
  virtual ~ISetupLog() {}
  virtual void line(const std::string& text) = 0;
};

enum class SetupStep { Running, Handback, Restart };

struct SetupSessionConfig {
  std::string ssid;
  std::string password;
  uint32_t graceMs = 1500;    // keep serving this long after a save so the browser gets its reply
  uint32_t retryMs = 5000;    // pause between failed access-point starts
  uint32_t serveCapMs = 0;    // 0 = serve forever (a factory node); else the fallback window's length
};

// The setup-mode state machine. Factory case: the node has nothing to join,
// so the setup WiFi serves FOREVER until the form saves (then: reply, tear
// down, restart) — a whole site's nodes can wait as long as it takes to be
// visited. Fallback case (credentials exist but none joined): serveCapMs
// bounds each window, and its expiry answers Handback so the sketch resumes
// the normal scan/connect flow and the setup WiFi cannot take a working
// node hostage when the outage was only temporary.
//
//   * The access point fails to start: log, clean up, try again later. NEVER
//     Handback in the factory case — a node with nothing to join must not
//     quietly go back to normal operation.
class SetupSession {
 public:
  SetupSession(const SetupSessionConfig& cfg, ISetupNet& net, const SetupForm& outcome,
               ISetupLog* log = nullptr)
      : cfg_(cfg), net_(net), outcome_(outcome), log_(log),
        state_(State::Starting) {}

  // Call it from the loop every pass; never blocks for long. Running: call
  // again. Handback: the setup WiFi is closed, resume the normal flow.
  // Restart: ESP.restart() — the saved credentials take over.
  SetupStep step(uint32_t nowMs) {
    if (state_ == State::Starting || state_ == State::RetryWait) return stepStart(nowMs);

    // Serving and Grace keep the pages flowing: without it the "saved,
    // restarting" reply would never reach the browser.
    net_.poll();
    if (state_ == State::Serving) {
      if (outcome_.saved()) {
        say("setup: saved, restarting");
        mark_ = nowMs;
        state_ = State::Grace;
        return SetupStep::Running;
      }
      if (cfg_.serveCapMs != 0 && static_cast<uint32_t>(nowMs - servingSinceMs_) >= cfg_.serveCapMs) {
        say("setup: nobody came, closing the setup WiFi for a while");
        stop();
        state_ = State::Done;
        return SetupStep::Handback;
      }
      return SetupStep::Running;
    }
    if (state_ == State::Grace) {
      if (static_cast<uint32_t>(nowMs - mark_) < cfg_.graceMs) return SetupStep::Running;
      stop();
      state_ = State::Done;
      return SetupStep::Restart;
    }
    return SetupStep::Running;  // Done: the caller acts on the last answer
  }

  // Leaves the WiFi clean. Safe to call at any time and more than once; a
  // session that never touched the network leaves it alone.
  void stop() {
    if (!touched_) return;
    touched_ = false;
    net_.stop();
  }

  bool serving() const { return state_ == State::Serving || state_ == State::Grace; }

 private:
  enum class State { Starting, RetryWait, Serving, Grace, Done };

  // Unsigned subtraction stays right across the ~49.7-day millis() wrap,
  // which a "serve forever" session will reach.
  static bool elapsed(uint32_t now, uint32_t since, uint32_t span) {
    return static_cast<uint32_t>(now - since) >= span;
  }

  void say(const std::string& text) {
    if (log_ != nullptr) log_->line(text);
  }

  SetupStep stepStart(uint32_t now) {
    if (state_ == State::RetryWait) {
      if (!elapsed(now, mark_, cfg_.retryMs)) return SetupStep::Running;
    }
    touched_ = true;
    if (net_.startAccessPoint(cfg_.ssid, cfg_.password)) {
      say("setup: WiFi '" + cfg_.ssid + "' is up - join it and open http://" + SETUP_AP_IP + "/");
      servingSinceMs_ = now;
      state_ = State::Serving;
      return SetupStep::Running;
    }
    net_.stop();  // a half-started access point would make the next attempt fail too
    say("setup: WiFi failed to start, will retry");
    mark_ = now;
    state_ = State::RetryWait;
    return SetupStep::Running;
  }

  SetupSessionConfig cfg_;
  ISetupNet& net_;
  const SetupForm& outcome_;
  ISetupLog* log_;
  State state_ = State::Starting;
  uint32_t mark_ = 0;
  uint32_t servingSinceMs_ = 0;
  bool touched_ = false;
};

// ── The request reader ──────────────────────────────────────────────────
// Reads ONE request, a few bytes at a time, and refuses everything unusual
// with the status to answer (400/411/413/431/501): no chunked bodies, no
// folded or bare-LF headers, no control characters, no body without
// Content-Length, nothing past the caps. Pure: no sockets, so it is tested
// byte by byte.
class HttpRequestReader {
 public:
  enum Status { NeedMore, Complete, Rejected };

  HttpRequestReader() { reset(); }

  void reset() {
    buf_.clear(); method_.clear(); target_.clear(); body_.clear(); host_.clear();
    headDone_ = false; contentLength_ = -1; rejectStatus_ = 0;
  }

  Status feed(const char* data, size_t n) {
    if (rejectStatus_) return Rejected;
    if (headDone_) {
      body_.append(data, n);
      return finishBody();
    }
    buf_.append(data, n);
    size_t end = buf_.find("\r\n\r\n");
    if (end == std::string::npos) {
      // no end of head yet: a head this long will never be accepted, so stop
      // collecting it. The 3 extra bytes are a blank line that has only
      // partly arrived ("\r\n\r"): they are not part of the head, so counting
      // them would refuse a head of 2045..2048 bytes or accept it depending
      // on how TCP cut it.
      if (buf_.size() > static_cast<size_t>(SETUP_HEAD_MAX) + 3) return reject(431);
      return NeedMore;
    }
    if (end > static_cast<size_t>(SETUP_HEAD_MAX)) return reject(431);
    std::string head = buf_.substr(0, end);
    body_ = buf_.substr(end + 4);
    buf_.clear();
    int status = parseHead(head);
    if (status) return reject(status);
    headDone_ = true;
    if (method_ == "POST" && contentLength_ < 0) return reject(411);
    if (contentLength_ > static_cast<long>(WIFI_SETUP_MAX_BODY)) return reject(413);  // refused before the body is waited for
    return finishBody();
  }

  int rejectStatus() const { return rejectStatus_; }
  const std::string& method() const { return method_; }
  const std::string& target() const { return target_; }
  const std::string& body() const { return body_; }
  const std::string& host() const { return host_; }

 private:
  static const size_t SETUP_HEAD_MAX = 2048;

  Status reject(int status) { rejectStatus_ = status; return Rejected; }

  Status finishBody() {
    size_t want = contentLength_ > 0 ? static_cast<size_t>(contentLength_) : 0;
    if (body_.size() < want) return NeedMore;
    body_.resize(want);  // the connection is closed after one answer: whatever follows is ignored
    return Complete;
  }

  static bool isControl(unsigned char c) { return (c < 0x20 && c != '\t') || c == 0x7f; }

  static std::string lower(std::string s) {
    for (size_t i = 0; i < s.size(); ++i) if (s[i] >= 'A' && s[i] <= 'Z') s[i] = static_cast<char>(s[i] - 'A' + 'a');
    return s;
  }

  // 0 = fine, else the status to answer
  int parseHead(const std::string& head) {
    // a CR or LF that is not part of a CRLF pair, or any other control byte,
    // is an attack or a bug
    for (size_t i = 0; i < head.size(); ++i) {
      unsigned char c = static_cast<unsigned char>(head[i]);
      if (c == '\r') { if (i + 1 >= head.size() || head[i + 1] != '\n') return 400; continue; }
      if (c == '\n') { if (i == 0 || head[i - 1] != '\r') return 400; continue; }
      if (isControl(c)) return 400;
    }
    size_t lineEnd = head.find("\r\n");
    int status = parseRequestLine(head.substr(0, lineEnd));
    if (status) return status;
    size_t pos = lineEnd == std::string::npos ? head.size() : lineEnd + 2;
    int headers = 0;
    while (pos < head.size()) {
      size_t e = head.find("\r\n", pos);
      std::string line = head.substr(pos, e == std::string::npos ? std::string::npos : e - pos);
      pos = e == std::string::npos ? head.size() : e + 2;
      if (++headers > 32) return 431;
      status = parseHeader(line);
      if (status) return status;
    }
    return 0;
  }

  int parseRequestLine(const std::string& line) {
    size_t a = line.find(' ');
    size_t b = a == std::string::npos ? a : line.find(' ', a + 1);
    if (b == std::string::npos) return 400;
    std::string method = line.substr(0, a), target = line.substr(a + 1, b - a - 1), version = line.substr(b + 1);
    if (method.empty() || method.size() > 8) return 400;
    for (size_t i = 0; i < method.size(); ++i) if (method[i] < 'A' || method[i] > 'Z') return 400;
    if (target.empty() || target.size() > 256 || target[0] != '/' || target.find(' ') != std::string::npos) return 400;
    // parseHead() lets a TAB through (header values may hold one); a request
    // target may not: it is where a "path" with a hidden separator would
    // slip past the exact-match routing of the form.
    for (size_t i = 0; i < target.size(); ++i) {
      if (static_cast<unsigned char>(target[i]) < 0x20 || static_cast<unsigned char>(target[i]) == 0x7f) return 400;
    }
    if (version != "HTTP/1.1" && version != "HTTP/1.0") return 400;
    method_ = method;
    target_ = target;  // with its query: the form cuts it off itself
    return 0;
  }

  int parseHeader(const std::string& line) {
    size_t colon = line.find(':');
    if (colon == std::string::npos || colon == 0) return 400;  // also catches a folded line (it has no name)
    std::string name = line.substr(0, colon);
    if (name.find(' ') != std::string::npos || name.find('\t') != std::string::npos) return 400;
    name = lower(name);
    std::string value = line.substr(colon + 1);
    size_t s = value.find_first_not_of(" \t"), e = value.find_last_not_of(" \t");
    value = s == std::string::npos ? std::string() : value.substr(s, e - s + 1);
    if (name == "transfer-encoding") return 501;  // chunked bodies are not read
    if (name == "host") { host_ = value; return 0; }
    if (name != "content-length") return 0;
    if (value.empty() || value.size() > 9) return 400;
    long n = 0;
    for (size_t i = 0; i < value.size(); ++i) {
      if (value[i] < '0' || value[i] > '9') return 400;  // no sign, no hex, no exponent
      n = n * 10 + (value[i] - '0');
    }
    if (contentLength_ >= 0 && contentLength_ != n) return 400;  // two different lengths: request smuggling
    contentLength_ = n;
    return 0;
  }

  std::string buf_, method_, target_, body_, host_;
  bool headDone_;
  long contentLength_;
  int rejectStatus_;
};

// ── The store-vs-compiled decision ──────────────────────────────────────
// How many usable networks the stored list holds: any invalid row means
// "damaged" and the answer is 0 (no credentials to join with). Zero feeds
// bootPortalDecision.
inline int effectiveNetworksCount(const WifiStoreNet* stored, int storedCount) {
  if (stored == nullptr || storedCount <= 0) return 0;
  // The store's rows arrive pre-validated (wifiStoreParse refuses the whole
  // blob on any bad entry); this re-check keeps the decision safe even
  // against a caller-built array.
  for (int i = 0; i < storedCount; ++i) {
    if (!validStoreSsid(stored[i].ssid)) return 0;
  }
  return storedCount;
}

// ── The boot decision (v2/v3): when the setup WiFi opens ────────────────
//   - setup ARMED (the dashboard's SETUP_MODE maintenance switch): always
//     portal — the operator deliberately asked for it, on any node state;
//   - NOT provisioned (the first successful WiFi connect never happened):
//     portal — the first install is still incomplete, so the captive page
//     is the node's only way forward (factory state, or credentials saved
//     but never joined with);
//   - NO relay configuration in flash (#140: host, port, key): portal —
//     the node has nowhere to report; this also serves the migration of a
//     provisioned v2 node whose relay settings were compiled in until now;
//   - provisioned WITH a relay configuration: never — a WiFi outage
//     retries with the normal flow; the captive portal must not reappear
//     where anyone nearby could join it.
enum class PortalDecision { Portal, Normal };

inline PortalDecision bootPortalDecision(bool setupArmed, bool provisioned, int usableNetworks,
                                         bool relayConfigured) {
  if (setupArmed) return PortalDecision::Portal;
  if (!provisioned) return PortalDecision::Portal;
  if (!relayConfigured) return PortalDecision::Portal;
  if (usableNetworks <= 0) return PortalDecision::Portal; // defensive: provisioned implies entries
  return PortalDecision::Normal;
}

// How long a portal boot may serve before it hands back to the normal flow
// (task #152). A FACTORY node (no usable credentials anywhere) has nothing
// to join — its only way forward is the form, so it serves FOREVER (0). A
// node that HOLDS credentials (the reboot right after a save, an armed
// maintenance window, the relay-missing migration) must get its normal
// scan/connect flow back: the first connect is what completes the install,
// and a forever-portal would capture the node so the saved network is never
// even tried — the exact "save and nothing happens" the bounded window
// exists to prevent. The sketch passes its SETUP_SERVE_MAX_MS as `boundedMs`.
inline uint32_t bootPortalServeCapMs(bool factoryEmpty, uint32_t boundedMs) {
  return factoryEmpty ? 0 : boundedMs;
}

} // namespace espnet
