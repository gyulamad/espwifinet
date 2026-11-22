// net.h — the node's connection to the relay: one outbound TCP connection,
// auth line, [4-byte big-endian length][JSON] framing (exactly the wire
// protocol the relay implements in nodejs/esp-relay/lib/protocol.js — the
// e2e test suite drives a fake node over the same bytes).
//
// Outbound-only is the architectural answer to "nodes may sit behind WiFi
// extenders or NAT routers": the node dials the relay, so no port forwarding
// or DMZ is needed anywhere, and the relay pushes commands (set_gpio,
// settings, ping) back down the same established connection.
//
// JSON handling: outgoing messages are built with string concatenation and
// incoming ones are read with tiny strict extractors below — all protocol
// values are machine-generated flat tokens (type/state/pin/true/false), so
// no JSON library dependency is needed on the node. If a future message
// needs free-form strings from unknown sources, switch to ArduinoJson then.

#pragma once

#include <Arduino.h>
#include <ESP8266WiFi.h>
#include "remote_log.h"

namespace espnet {

const uint32_t CONNECT_BACKOFF_MIN_MS = 1000;
const uint32_t CONNECT_BACKOFF_MAX_MS = 30000;
// If the relay stays silent this long (status cadence is ~10 s, pings 15 s),
// the TCP link is half-open behind some NAT box: drop it and redial.
const uint32_t RELAY_SILENT_LIMIT_MS = 60000;

// ── tiny flat-JSON readers (see the file header) ──
// Finds `"key"` followed by ':' and copies the string value (no escapes —
// protocol values never contain one). Returns false when absent.
bool jsonGetString(const char* json, const char* key, char* out, size_t outLen) {
  char pattern[40];
  snprintf(pattern, sizeof(pattern), "\"%s\":", key);
  const char* p = strstr(json, pattern);
  if (p == nullptr) return false;
  p += strlen(pattern);
  if (*p != '"') return false;
  p++;
  size_t i = 0;
  while (*p != '"' && *p != '\0' && i + 1 < outLen) {
    out[i++] = *p++;
  }
  out[i] = '\0';
  return *p == '"';
}

bool jsonGetInt(const char* json, const char* key, long& out) {
  char buf[16];
  if (!jsonGetString(json, key, buf, sizeof(buf))) {
    // numbers are unquoted: find the pattern then parse digits directly
    char pat[40];
    snprintf(pat, sizeof(pat), "\"%s\":", key);
    const char* p = strstr(json, pat);
    if (p == nullptr) return false;
    p += strlen(pat);
    if (*p == '"') return false; // quoted but failed the string read above
    out = strtol(p, nullptr, 10);
    return true;
  }
  out = strtol(buf, nullptr, 10);
  return true;
}

bool jsonGetBool(const char* json, const char* key, bool& out) {
  char pat[40];
  snprintf(pat, sizeof(pat), "\"%s\":", key);
  const char* p = strstr(json, pat);
  if (p == nullptr) return false;
  p += strlen(pat);
  if (strncmp(p, "true", 4) == 0) { out = true; return true; }
  if (strncmp(p, "false", 5) == 0) { out = false; return true; }
  return false;
}

// ── outgoing framing ──
void sendMessage(WiFiClient& client, const char* json) {
  uint8_t head[4];
  const size_t len = strlen(json);
  head[0] = (uint8_t)((len >> 24) & 0xff);
  head[1] = (uint8_t)((len >> 16) & 0xff);
  head[2] = (uint8_t)((len >> 8) & 0xff);
  head[3] = (uint8_t)(len & 0xff);
  client.write(head, 4);
  client.write((const uint8_t*)json, len);
  client.flush();
}

// Callbacks the RelayConnection invokes so the sketch owns the behaviour.
struct NetHooks {
  void (*onSettings)(long gpioPin, bool activeHigh, long statusMs) = nullptr;
  void (*onSetGpio)(const char* state, long pin, bool activeHigh) = nullptr;
};

class RelayConnection {
 public:
  // clientOverride lets the desktop tests feed a stub WiFiClient; production
  // sketches call begin() without it and use the internal client.
  void begin(const char* host, uint16_t port, const char* deviceId,
             const char* apiKey, RemoteLog* log, NetHooks hooks,
             WiFiClient* clientOverride = nullptr) {
    host_ = host;
    port_ = port;
    deviceId_ = deviceId;
    apiKey_ = apiKey;
    log_ = log;
    hooks_ = hooks;
    client_ = (clientOverride != nullptr) ? clientOverride : &ownedClient_;
  }

  bool connected() const { return client_->connected(); }

  // Drops the link and clears the receive buffer (used on half-open links
  // and protocol errors); the next ensureRelay() dials a fresh connection.
  void reset() {
    client_->stop();
    buffer_ = "";
    lastFromRelayMs_ = millis();
  }

  // Dials the relay, authenticates and (on success) says hello. Non-blocking
  // enough for loop(): one attempt per call, backoff handled by the caller.
  bool connectAndSayHello(const char* fwVersion, const char* chip) {
    if (client_->connected()) return true;
    if (!client_->connect(host_, port_)) return false;

    // 1. the one-time auth line: "<deviceId>\t<apiKey>\n"
    client_->print(deviceId_);
    client_->print('\t');
    client_->print(apiKey_);
    client_->print('\n');

    // 2. hello: who we are (the relay answers with settings + desired gpio)
    char msg[192];
    snprintf(msg, sizeof(msg),
             "{\"type\":\"hello\",\"fw\":\"%s\",\"chip\":\"%s\",\"reset\":\"%s\"}",
             fwVersion, chip, ESP.getResetReason().c_str());
    sendMessage(*client_, msg);
    lastFromRelayMs_ = millis();
    return true;
  }

  // Status report (WiFi SSID + signal strength end up on the dashboard).
  void sendStatus(long gpioPin, const char* gpioState) {
    if (!client_->connected()) return;
    char msg[224];
    snprintf(msg, sizeof(msg),
             "{\"type\":\"status\",\"ssid\":\"%s\",\"rssi\":%ld,\"heapFree\":%lu,"
             "\"uptimeMs\":%lu,\"ip\":\"%s\",\"gpioPin\":%ld,\"gpioState\":\"%s\"}",
             WiFi.SSID().c_str(), (long)WiFi.RSSI(), (unsigned long)ESP.getFreeHeap(),
             (unsigned long)millis(), WiFi.localIP().toString().c_str(), gpioPin, gpioState);
    sendMessage(*client_, msg);
  }

  void sendLogLine(const LogEntry& entry) {
    if (!client_->connected()) return;
    char msg[LOG_MSG_MAX + 64];
    snprintf(msg, sizeof(msg),
             "{\"type\":\"log\",\"level\":\"%s\",\"msg\":\"[%08lu] %s\"}",
             entry.level == LOG_WARN ? "warn" : (entry.level == LOG_ERROR ? "error" : "info"),
             (unsigned long)entry.ms, entry.msg);
    sendMessage(*client_, msg);
  }

  void sendGpioAck(const char* state, long pin) {
    if (!client_->connected()) return;
    char msg[96];
    snprintf(msg, sizeof(msg), "{\"type\":\"gpio_ack\",\"state\":\"%s\",\"pin\":%ld}", state, pin);
    sendMessage(*client_, msg);
  }

  void sendPong() {
    if (!client_->connected()) return;
    sendMessage(*client_, "{\"type\":\"pong\"}");
  }

  uint32_t silentMs() const { return millis() - lastFromRelayMs_; }

  // Reads and dispatches whatever arrived since the last call. Returns false
  // on a protocol error (the caller should drop the connection).
  bool pumpMessages() {
    while (client_->available() > 0) {
      buffer_ += (char)client_->read();
      // try to parse complete messages out of the buffer
      for (;;) {
        if (buffer_.length() < 4) break;
        const uint32_t len = ((uint32_t)(uint8_t)buffer_[0] << 24)
                           | ((uint32_t)(uint8_t)buffer_[1] << 16)
                           | ((uint32_t)(uint8_t)buffer_[2] << 8)
                           | (uint32_t)(uint8_t)buffer_[3];
        if (len == 0 || len > 4096) {
          if (log_ != nullptr) log_->logf(LOG_ERROR, "protocol: bad length %lu", (unsigned long)len);
          return false;
        }
        if (buffer_.length() < 4 + len) break; // wait for the rest
        String payload = buffer_.substring(4, 4 + len);
        buffer_.remove(0, 4 + len);
        lastFromRelayMs_ = millis();
        if (!dispatch(payload.c_str())) return false;
      }
      // A malformed sender could grow buffer_ forever; cap it.
      if (buffer_.length() > 8192) {
        if (log_ != nullptr) log_->logf(LOG_ERROR, "protocol: buffer overflow");
        return false;
      }
    }
    return true;
  }

 private:
  bool dispatch(const char* json) {
    char type[24];
    if (!jsonGetString(json, "type", type, sizeof(type))) {
      if (log_ != nullptr) log_->logf(LOG_WARN, "protocol: message without type");
      return true; // drop it, keep the link
    }
    if (strcmp(type, "ping") == 0) {
      sendPong();
      return true;
    }
    if (strcmp(type, "settings") == 0) {
      long pin = -1;
      long statusMs = -1;
      bool activeHigh = false;
      jsonGetInt(json, "gpioPin", pin);
      jsonGetInt(json, "statusIntervalMs", statusMs);
      jsonGetBool(json, "gpioActiveHigh", activeHigh);
      if (hooks_.onSettings != nullptr) hooks_.onSettings(pin, activeHigh, statusMs);
      return true;
    }
    if (strcmp(type, "set_gpio") == 0) {
      char state[8];
      long pin = -1;
      bool activeHigh = true;
      if (!jsonGetString(json, "state", state, sizeof(state))) {
        if (log_ != nullptr) log_->logf(LOG_WARN, "set_gpio without state");
        return true;
      }
      jsonGetInt(json, "pin", pin);
      jsonGetBool(json, "activeHigh", activeHigh);
      if (hooks_.onSetGpio != nullptr) hooks_.onSetGpio(state, pin, activeHigh);
      return true;
    }
    // unknown types are ignored (forward compatibility)
    return true;
  }

  WiFiClient ownedClient_;
  WiFiClient* client_ = &ownedClient_;
  String buffer_;
  const char* host_ = nullptr;
  uint16_t port_ = 0;
  const char* deviceId_ = nullptr;
  const char* apiKey_ = nullptr;
  RemoteLog* log_ = nullptr;
  NetHooks hooks_;
  uint32_t lastFromRelayMs_ = 0;
};

} // namespace espnet
