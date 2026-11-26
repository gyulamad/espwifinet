#!/usr/bin/env bash
# run_tests.sh — the full test suite for the ESP Wifi Network base project.
#   1. node unit tests (relay logic)
#   2. node e2e test (spawns the real relay, drives it over TCP + HTTP)
#   3. C++ logic tests (node firmware's net/log/ota logic against stubs, g++)
#   4. firmware stack budget (frame sizes, tests/stack_budget.sh, g++)
#
# Usage: ./run_tests.sh   (installs the relay's npm dependencies if missing)
set -u

fail=0

echo "── relay dependencies ──"
if [ ! -d nodejs/esp-relay/node_modules ]; then
  (cd nodejs/esp-relay && npm install --no-audit --no-fund --omit=dev) || fail=1
else
  echo "node_modules present, skipping npm install"
fi

echo "── node unit tests ──"
(cd nodejs/esp-relay && node tests/run_all.js) || fail=1

echo ""
echo "── node e2e test ──"
(cd nodejs/esp-relay && node tests/e2e/test_relay_e2e.js) || fail=1

if command -v g++ >/dev/null 2>&1; then
  # #131: the sketch is one source tree for both boards — the desktop build
  # compiles the REAL headers once per board (the ESP8266 core defines
  # ESP8266, the ESP32 core ESP32; the stubs emulate both radios).
  echo ""
  echo "── C++ logic tests (ESP8266 branch) ──"
  tmpbin="$(mktemp -d)/test_net_logic_esp8266"
  if g++ -std=gnu++17 -Wall -Wextra -DESP8266 -Inodejs 2>/dev/null -Itests/cpp/stubs \
      tests/cpp/test_net_logic.cpp -o "$tmpbin"; then
    "$tmpbin" || fail=1
  else
    echo "FAIL C++ build (esp8266)"
    fail=1
  fi
  echo ""
  echo "── C++ logic tests (ESP32 branch) ──"
  tmpbin32="$(mktemp -d)/test_net_logic_esp32"
  if g++ -std=gnu++17 -Wall -Wextra -DESP32 -Inodejs 2>/dev/null -Itests/cpp/stubs/esp32 -Itests/cpp/stubs \
      tests/cpp/test_net_logic.cpp -o "$tmpbin32"; then
    "$tmpbin32" || fail=1
  else
    echo "FAIL C++ build (esp32)"
    fail=1
  fi
else
  echo ""
  echo "── C++ logic tests skipped (no g++) ──"
fi

# #152: the firmware's stack frames stay small. The loop task has ~4 KB of
# stack on the ESP8266 and the desktop tests run on 8 MB, so an oversized
# local (the 3000-byte flash blob that overflowed it on the first Save) is
# invisible to every assert - this step measures the frames instead.
echo ""
echo "── firmware stack budget (#152) ──"
bash tests/stack_budget.sh || fail=1

# #144: the dashboard's PHP side now has its own coverage - a syntax lint of
# every php file plus a real e2e of setup.php's first-run write (the
# read-only-folder failure and the success path), driven through php -S.
# Skipped gracefully on machines without php/curl.
echo ""
echo "── php dashboard: lint + setup e2e (#144) ──"
if command -v php >/dev/null 2>&1; then
  find php -name '*.php' -exec php -l {} \; | grep -v "No syntax errors" && fail=1
  if command -v curl >/dev/null 2>&1; then
    bash tests/php/test_setup_e2e.sh || fail=1
  else
    echo "setup e2e skipped (no curl)"
  fi
else
  echo "php checks skipped (no php)"
fi

echo ""
if [ "$fail" -eq 0 ]; then
  echo "ALL TESTS PASSED"
else
  echo "TESTS FAILED"
fi
exit "$fail"
