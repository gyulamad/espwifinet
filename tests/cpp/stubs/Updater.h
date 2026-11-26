// Updater.h stub — the ESP8266 branch of the desktop build (task #165): the
// ESP8266 core ships its flasher in Updater.h, and — both on 2.7.4 and on
// 3.x — declares the object `Update` (extern UpdaterClass Update;), the SAME
// name the ESP32 core uses in Update.h. The first stub version called the
// object `Updater`, which no core declares: the desktop build compiled the
// mirror of a mirror and passed while the real NodeMCU 1.0 build failed with
// "'Updater' was not declared in this scope; did you mean 'Update'?" — the
// compiler pointing at the right name. The core's real surface differences
// are kept on purpose (no abort() here — clearError() drops a failed
// attempt, and the error getter is getErrorString()): if ota.h's flow ever
// calls the ESP32-only call again, the ESP8266 desktop build fails to
// compile instead of hiding it. See Update.h for the ESP32 stub.
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
extern UpdaterStub Update;   // the real core: extern UpdaterClass Update;
