// EEPROM.h stub — the emulated EEPROM both Arduino cores provide with the
// same API. The node's credential and relay stores (#135/#140,
// wifi_store_hw.h) keep their regions here.
//
// The two cores DIFFER in the one place that bit the node (task #152), and
// the stub reproduces both so the desktop build catches it:
//   - ESP8266: one flash sector. begin(size) only sizes the RAM copy; the
//     flash content is never resized.
//   - ESP32: the "EEPROM" is ONE NVS blob. begin(size) RESIZES that blob in
//     flash, right there: a size SMALLER than the stored blob truncates it
//     for good (the bytes beyond are lost), a bigger one expands it with
//     0xFF (arduino-esp32 libraries/EEPROM/src/EEPROM.cpp). Two begin()
//     calls with different sizes (3000, then 4096) therefore erased the
//     relay region (offset 3000+) on every boot.
// powerCycle() drops the RAM copy and keeps the flash, like a restart.

#pragma once

#include <cstring>

#include "Arduino.h"

class EEPROMClass {
 public:
  static const size_t kSector = 4096;

  EEPROMClass() { memset(flash_, 0xFF, sizeof(flash_)); memset(mem_, 0, sizeof(mem_)); }

  bool begin(size_t size) {
    if (size == 0 || size > kSector) return false;
#if defined(ESP32)
    if (size < flashLen_) {                 // truncate: the tail is gone for good
      memset(flash_ + size, 0xFF, flashLen_ - size);
      flashLen_ = size;
    } else if (size > flashLen_) {          // new or expanded: fresh bytes are 0xFF
      memset(flash_ + flashLen_, 0xFF, size - flashLen_);
      flashLen_ = size;
    }
#endif
    size_ = size;
    memcpy(mem_, flash_, size_);
    return true;
  }
  uint8_t read(int i) { return (i >= 0 && static_cast<size_t>(i) < size_) ? mem_[i] : 0; }
  void write(int i, uint8_t v) { if (i >= 0 && static_cast<size_t>(i) < size_) mem_[i] = v; }
  bool commit() {
    if (size_ == 0) return false;
    memcpy(flash_, mem_, size_);
#if defined(ESP32)
    flashLen_ = size_;                      // the blob is rewritten at the size begin() set
#endif
    return true;
  }

  // test control: a restart loses the RAM copy, the flash stays
  void powerCycle() { size_ = 0; memset(mem_, 0, sizeof(mem_)); }

  uint8_t mem_[kSector];
  uint8_t flash_[kSector];
  size_t size_ = 0;
  size_t flashLen_ = 0;  // ESP32: the NVS blob's current length
};

extern EEPROMClass EEPROM;  // the test TU defines the global object
