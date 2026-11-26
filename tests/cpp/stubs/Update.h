// Update.h stub — the ESP32 branch of the desktop build (task #165): the
// ESP32 core ships its flasher as `Update` (extern UpdateClass Update; in
// Update.h). The ESP8266 core ships the flasher under the SAME object name
// in <Updater.h> — see Updater.h for that stub. Like every stub here, only
// the surface ota.h uses: begin() is steerable so the desktop test can walk
// the flash failure path. errorString() mirrors the real core's
// const char* errorString() (the ESP8266 core's getter is instead
// String getErrorString() — ota.h's flasherError() normalizes both to
// String).
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
