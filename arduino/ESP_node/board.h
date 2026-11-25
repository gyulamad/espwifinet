// board.h — the one place that knows which board the sketch compiles for
// (#131). Both the ESP8266 and the ESP32 Arduino core define their macro
// (ESP8266 / ESP32), so the includes and the few board-specific calls
// (chip id, watchdog, GPIO ceiling) resolve at compile time; everything
// else — the relay protocol, the WiFi roam policy, the OTA flow, the log
// queue — is board-agnostic and shared unchanged. The sketch keeps ONE
// source tree for both boards (the folder is ESP_node, not per-board).
//
// ESP8266 (NodeMCU 1.0 / Wemos D1 mini, the tested target):
//   - output-capable GPIO legs 0..16,
//   - the software watchdog needs an explicit feed from loop(),
//   - chip id: ESP.getChipId().
// ESP32 / WROOM32:
//   - output-capable legs are 0..33; GPIO 6..11 are wired to the flash
//     chip and must never be driven, 34..39 are input-only — a pushed pin
//     outside the usable set keeps the node on its current leg instead of
//     bricking or silently doing nothing,
//   - the loop task's watchdog is not enabled by default on the Arduino
//     core, so feeding is a no-op (the core yields between loop() passes),
//   - there is no getChipId(): the lower 32 bits of the eFuse MAC take
//     over — still unique per board out of the box.

#pragma once

#include <Arduino.h>

#if defined(ESP32)

#include <WiFi.h>
#include <HTTPClient.h>
#include <esp_system.h> // esp_reset_reason(): the ESP32 core has no ESP.getResetReason()

#define ESPNET_CHIP_NAME "esp32"
#define ESPNET_FIRMWARE_PATH "/firmware/node-esp32.bin"

namespace espnet {

const int GPIO_PIN_MAX = 33;

inline bool gpioPinAllowed(long pin) {
  if (pin < 0 || pin > GPIO_PIN_MAX) return false;
  return !(pin >= 6 && pin <= 11); // 6..11: the flash chip's own pins
}

inline void feedWdt() {} // the loop task's watchdog is off by default

inline uint32_t chipId() { return (uint32_t)(ESP.getEfuseMac() & 0xffffffffULL); }

// The ESP32 core's EspClass has no getResetReason() (that member is
// ESP8266-only - the v1 desktop stub hid the difference because its fake ESP
// class carried the ESP8266 surface on both builds, so the real esp32 compile
// caught what the suite could not); the reason comes from the system layer
// instead, mapped to the same short words the ESP8266 reports.
inline String resetReason() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:   return String("Power on");
    case ESP_RST_SW:        return String("Software restart");
    case ESP_RST_PANIC:     return String("Exception");
    case ESP_RST_INT_WDT:   return String("Interrupt Watchdog");
    case ESP_RST_TASK_WDT:  return String("Task Watchdog");
    case ESP_RST_WDT:       return String("Hardware Watchdog");
    case ESP_RST_DEEPSLEEP: return String("Deep-Sleep Wake");
    case ESP_RST_BROWNOUT:  return String("Brownout");
    case ESP_RST_SDIO:      return String("SDIO Reset");
    default:                return String("Unknown");
  }
}

} // namespace espnet

#elif defined(ESP8266)

#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>

#define ESPNET_CHIP_NAME "esp8266"
#define ESPNET_FIRMWARE_PATH "/firmware/node.bin"

namespace espnet {

const int GPIO_PIN_MAX = 16;

inline bool gpioPinAllowed(long pin) { return pin >= 0 && pin <= GPIO_PIN_MAX; }

inline void feedWdt() { ESP.wdtFeed(); }

inline uint32_t chipId() { return ESP.getChipId(); }

inline String resetReason() { return ESP.getResetReason(); }

} // namespace espnet

#else

#error "ESP_node targets the ESP8266 and the ESP32 Arduino cores - select the board in the IDE and the core defines ESP8266 or ESP32 itself."

#endif
