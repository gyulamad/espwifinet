// wifi_store_hw.h — the flash half of the WiFi credential store (task
// #135): the ESP8266 and the ESP32 Arduino cores both emulate EEPROM with
// the same API (ESP8266: one flash sector; ESP32: NVS-backed), so one
// backend serves both boards and no per-board branch is needed. The store
// reserves the FIRST WIFI_STORE_REGION_MAX bytes of the EEPROM space; the
// sketch uses no other EEPROM content.
//
// Glue only: never compiled on the desktop (the desktop tests run
// wifi_store.h against a RAM backend), so including <EEPROM.h> here is safe.

#pragma once

#include <EEPROM.h>

#include "wifi_store.h"

namespace espnet {

class EepromWifiStoreBackend : public WifiStoreBackend {
 public:
  void begin() { EEPROM.begin(WIFI_STORE_REGION_MAX); }

  bool read(char* buf, size_t max) override {
    if (buf == nullptr || max < WIFI_STORE_REGION_MAX) return false;
    for (size_t i = 0; i < WIFI_STORE_REGION_MAX; ++i) {
      buf[i] = static_cast<char>(EEPROM.read(static_cast<int>(i)));
    }
    return true;
  }

  bool write(const char* buf, size_t len) override {
    if (buf == nullptr || len == 0 || len > WIFI_STORE_REGION_MAX) return false;
    for (size_t i = 0; i < len; ++i) {
      EEPROM.write(static_cast<int>(i), buf[i]);
    }
    return EEPROM.commit();
  }
};

} // namespace espnet
