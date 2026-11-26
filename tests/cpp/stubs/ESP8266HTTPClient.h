// ESP8266HTTPClient.h stub — only the surface ota.h uses. begin() succeeds;
// the GET reply is steerable for the desktop tests (task #165): the defaults
// keep every reply failing (the OTA tick's retry path, the original stub
// behavior) and the flash-failure test flips the knobs so tick() reaches
// the flash step; the happy path is covered server-side by the relay e2e
// suite.

#pragma once

#include "Arduino.h"

class HTTPClient {
 public:
  bool begin(WiFiClient&, const String&) { return true; }
  void setTimeout(int) {}
  int GET() { return code; }
  int getSize() { return size; }
  String getString() { return body; }
  void end() {}
  inline static int code = 404;
  inline static int size = -1;
  inline static String body;
};
