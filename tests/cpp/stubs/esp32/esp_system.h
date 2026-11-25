// esp_system.h stub — the ESP32 desktop build's fake of the real core's
// esp_reset_reason() surface (esp_system.h), which board.h's ESP32 branch
// consumes: the ESP32 core's EspClass has no getResetReason() (#131 v2),
// the reset reason comes from here. The tests steer the answer through
// espResetReasonState().

#pragma once

// The real esp_reset_reason_t enumerators (esp_system.h, stable across the
// Arduino-ESP32 2.x/3.x cores).
enum esp_reset_reason_t {
  ESP_RST_UNKNOWN = 0,
  ESP_RST_POWERON,
  ESP_RST_SW,
  ESP_RST_PANIC,
  ESP_RST_INT_WDT,
  ESP_RST_TASK_WDT,
  ESP_RST_WDT,
  ESP_RST_DEEPSLEEP,
  ESP_RST_BROWNOUT,
  ESP_RST_SDIO,
};

inline int& espResetReasonState() { static int v = ESP_RST_SW; return v; }
inline int esp_reset_reason() { return espResetReasonState(); }

// The hardware RNG the setup glue's CSRF token draws from (setup_ap.h, #135).
inline uint32_t esp_random() { return 0x5EEDF00Du; }
