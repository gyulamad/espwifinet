// Updater.h stub — the ESP8266 branch of the desktop build (task #165): the
// ESP8266 core ships its flasher as Updater (Updater.h) with a differently
// named error getter and NO abort() (clearError() drops a failed attempt) —
// exactly the surface difference the NodeMCU 1.0 compile exposed. The stub
// mirrors the real core's surface on purpose (no abort() here): if ota.h's
// flow ever calls the ESP32-only call again, the ESP8266 desktop build
// fails to compile instead of hiding it. See Update.h for the ESP32 stub.
#pragma once
#include "WiFiClient.h"

class UpdaterStub {
 public:
  bool begin(size_t) { return beginSucceeds; }
  size_t writeStream(WiFiClient&) { return written; }
  bool end(bool) { return endSucceeds; }
  void clearError() {}
  const char* getErrorString() { return "stub: not on hardware"; }
  inline static bool beginSucceeds = false;
  inline static size_t written = 0;
  inline static bool endSucceeds = false;
};
extern UpdaterStub Updater;
