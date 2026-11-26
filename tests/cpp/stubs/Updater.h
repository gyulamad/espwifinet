// Updater.h stub — the ESP8266 branch of the desktop build (task #165): the
// ESP8266 core ships its flasher in Updater.h, and — both on 2.7.4 and on
// 3.x — declares the object `Update` (extern UpdaterClass Update;), the SAME
// name the ESP32 core uses in Update.h. The first stub version called the
// object `Updater`, which no core declares: the desktop build compiled the
// mirror of a mirror and passed while the real NodeMCU 1.0 build failed with
// "'Updater' was not declared in this scope; did you mean 'Update'?" — the
// compiler pointing at the right name. The same discipline now covers the
// error getter: the real core declares String getErrorString() — an earlier
// attempt mirrored the NAME but typed the return const char*, so the real
// NodeMCU build failed with "cannot convert 'String' to 'const char*' in
// return" while the desktop suite kept passing. This stub returns the core's
// String, so that mismatch again fails here instead of on the board. The
// other real-surface differences are kept on purpose (no abort() here —
// clearError() drops a failed attempt). See Update.h for the ESP32 stub.
#pragma once
#include "WiFiClient.h"

class UpdaterStub {
 public:
  bool begin(size_t) { return beginSucceeds; }
  size_t writeStream(WiFiClient&) { return written; }
  bool end(bool) { return endSucceeds; }
  void clearError() {}
  String getErrorString() const { return String("stub: not on hardware"); }
  inline static bool beginSucceeds = false;
  inline static size_t written = 0;
  inline static bool endSucceeds = false;
};
extern UpdaterStub Update;   // the real core: extern UpdaterClass Update;
