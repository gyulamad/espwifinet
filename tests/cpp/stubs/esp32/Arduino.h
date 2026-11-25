// esp32/Arduino.h — the first include point of the ESP32 desktop build (the
// run_tests.sh -I order puts this directory before the shared stubs). The
// shared stub mirrors the ESP8266 core's surface; the real ESP32 core differs
// in exactly the places that matter:
//   - NO min()/max() macros: calls resolve to std::min and both sides must
//     deduce the same type — the v1 stub's macros hid the mixed-type min() in
//     wifi.h that the real esp32 toolchain refused (#131 v2),
//   - EspClass has no getResetReason(); the system layer's esp_reset_reason()
//     is the API (board.h maps it to the ESP8266 wording),
//   - F() is the identity (the ESP32 has no separate flash string space).
// Riding the real core's surface is what lets the desktop build catch the
// error class the hardware toolchain caught.

#pragma once

#include "../Arduino.h"
#include "esp_system.h"

#undef min
#undef max

#define F(x) (x)

// The real ESP32 core pulls <algorithm> in and lifts std::min/std::max into
// the global namespace (that is how sketches call bare min()/max() there, as
// template overloads - no macro, so mixed argument types are a compile
// error, exactly what the real esp32 toolchain caught in v1).
using std::min;
using std::max;
