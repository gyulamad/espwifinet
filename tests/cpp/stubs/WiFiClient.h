// WiFiClient.h stub for the desktop build of the node's network headers
// (ota.h, net.h and friends). The flasher stubs live in their own headers,
// named after the real core header they fake (task #165): Update.h for the
// ESP32 branch, Updater.h for the ESP8266 branch — ota.h includes exactly
// the one its compiled board carries, like it does against the real cores.

#pragma once

#include "ESP8266WiFi.h"

#include "ESP8266HTTPClient.h"
