// wifi_store_hw.h — the flash half of the node's credential and relay
// configuration stores (tasks #135, #140): the ESP8266 and the ESP32
// Arduino cores both emulate EEPROM with the same API (ESP8266: one flash
// sector; ESP32: NVS-backed), so one backend serves both boards and no
// per-board branch is needed. The EEPROM space is split: the WiFi store at
// offset 0 (WIFI_STORE_REGION_MAX bytes) and the relay configuration at
// RELAY_CONFIG_BASE (task #140). The sketch uses no other EEPROM content.
//
// Glue only: never compiled on the desktop (the desktop tests run
// wifi_store.h against a RAM backend), so including <EEPROM.h> here is safe.

#pragma once

#include <EEPROM.h>

#include "wifi_store.h"

namespace espnet {

// One EEPROM region backend over [base, base+size) of the EEPROM address
// space. Every write commits immediately, so the two backends the sketch
// creates never hold uncommitted data across one another's begin().
class EepromRegionBackend : public WifiStoreBackend {
 public:
  EepromRegionBackend(size_t base, size_t size) : base_(base), size_(size) {}

  // Starts the shared EEPROM at the size of the WHOLE space (both regions),
  // whichever backend asks: EEPROM.begin() resizes the stored blob on the
  // ESP32, so a smaller size than the one stored would truncate the other
  // region away (task #152, see EEPROM_TOTAL_SIZE). The same size twice is a
  // no-op for the blob (and every write here commits at once on both cores).
  void begin() { EEPROM.begin(static_cast<size_t>(EEPROM_TOTAL_SIZE)); }

  bool read(char* buf, size_t max) override {
    if (buf == nullptr || max < size_) return false;
    for (size_t i = 0; i < size_; ++i) {
      buf[i] = static_cast<char>(EEPROM.read(static_cast<int>(base_ + i)));
    }
    return true;
  }

  bool write(const char* buf, size_t len) override {
    if (buf == nullptr || len == 0 || len > size_) return false;
    for (size_t i = 0; i < len; ++i) {
      EEPROM.write(static_cast<int>(base_ + i), buf[i]);
    }
    return EEPROM.commit();
  }

 private:
  size_t base_;
  size_t size_;
};

} // namespace espnet
