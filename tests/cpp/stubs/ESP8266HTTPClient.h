// ESP8266HTTPClient.h stub — only the surface ota.h uses. begin() succeeds,
// GET() always fails with a non-200 code so the OTA tick exercises its
// retry path; the happy path is covered server-side by the relay e2e suite.

#pragma once

#include "Arduino.h"

class HTTPClient {
 public:
  bool begin(WiFiClient&, const String&) { return true; }
  void setTimeout(int) {}
  int GET() { return 404; }
  int getSize() { return -1; }
  String getString() { return String(); }
  void end() {}
};
