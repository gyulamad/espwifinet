// WiFiClient.h / Update.h stubs for the desktop build of ota.h.
// (The ESP8266 core ships both; the stubs only need to compile ota.h with
// the failure paths intact.)

#pragma once

#include "ESP8266WiFi.h"

class UpdateStub {
 public:
  bool begin(size_t) { return false; }
  size_t writeStream(WiFiClient&) { return 0; }
  bool end(bool) { return false; }
  void abort() {}
  const char* errorString() { return "stub: not on hardware"; }
};
extern UpdateStub Update;

#include "ESP8266HTTPClient.h"
