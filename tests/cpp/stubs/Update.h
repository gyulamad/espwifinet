// Update.h stub — the ESP32 branch of the desktop build (task #165): the
// ESP32 core ships its flasher as Update (Update.h); the ESP8266 branch
// gets Updater (Updater.h) instead. Like every stub here, only the surface
// ota.h uses: begin() is steerable so the desktop test can walk the flash
// failure path (see Updater.h for the mirrored ESP8266 stub).
#pragma once
#include "WiFiClient.h"

class UpdateStub {
 public:
  bool begin(size_t) { return beginSucceeds; }
  size_t writeStream(WiFiClient&) { return written; }
  bool end(bool) { return endSucceeds; }
  void abort() {}
  const char* errorString() { return "stub: not on hardware"; }
  inline static bool beginSucceeds = false;
  inline static size_t written = 0;
  inline static bool endSucceeds = false;
};
extern UpdateStub Update;
