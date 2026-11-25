// esp8266_peri.h stub — RANDOM_REG32, the hardware RNG register the setup
// glue's CSRF token draws from (setup_ap.h, #135). A deterministic xorshift
// stands in for the register: the value only has to exist and vary.
#pragma once

#include <cstdint>

inline uint32_t espRandomRegStub() {
  static uint32_t s = 0x5EEDF00Du;
  s ^= s << 13;
  s ^= s >> 17;
  s ^= s << 5;
  return s;
}

#define RANDOM_REG32 espRandomRegStub()
