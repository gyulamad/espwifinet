// EEPROM.h stub — the emulated EEPROM both Arduino cores provide with the
// same API (ESP8266: one flash sector; ESP32: NVS-backed). The node's
// credential store (#135, wifi_store_hw.h) keeps its region here. The stub's
// 512 bytes start zeroed, which the store reads as "blank".
#pragma once

#include "Arduino.h"

class EEPROMClass {
 public:
  void begin(size_t) {}
  uint8_t read(int i) { return (i >= 0 && i < 512) ? mem_[i] : 0; }
  void write(int i, uint8_t v) { if (i >= 0 && i < 512) mem_[i] = v; }
  bool commit() { return true; }

  uint8_t mem_[512] = {0};
};

extern EEPROMClass EEPROM;  // the test TU defines the global object
