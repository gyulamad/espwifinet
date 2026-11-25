// DNSServer.h stub — the captive-portal DNS the setup glue (#135) drives;
// both cores ship this class with the same API.
#pragma once

#include "Arduino.h"

// The real DNSServer.h's reply-code enum (the glue sets NoError so every
// name resolves to the board: phones then open the setup page by themselves).
// Unscoped, as on the cores - the glue can still write DNSReplyCode::NoError
// (enumerator qualification of an unscoped enum, C++11 and later).
enum DNSReplyCode {
  NoError = 0,
  FormError = 1,
  ServerError = 2,
  NonAuthoritative = 3,
  Refused = 5,
};

class DNSServer {
 public:
  void setErrorReplyCode(DNSReplyCode) {}
  bool start(uint16_t, const char*, const IPAddress&) { return true; }
  void processNextRequest() {}
  void stop() {}
};
