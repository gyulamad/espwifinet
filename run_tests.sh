#!/usr/bin/env bash
# run_tests.sh — the full test suite for the ESP Wifi Network base project.
#   1. node unit tests (relay logic)
#   2. node e2e test (spawns the real relay, drives it over TCP + HTTP)
#   3. C++ logic tests (node firmware's net/log/ota logic against stubs, g++)
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
  echo ""
  echo "── C++ logic tests ──"
  tmpbin="$(mktemp -d)/test_net_logic"
  if g++ -std=gnu++17 -Wall -Wextra -Inodejs 2>/dev/null -Itests/cpp/stubs \
      tests/cpp/test_net_logic.cpp -o "$tmpbin"; then
    "$tmpbin" || fail=1
  else
    echo "FAIL C++ build"
    fail=1
  fi
else
  echo ""
  echo "── C++ logic tests skipped (no g++) ──"
fi

echo ""
if [ "$fail" -eq 0 ]; then
  echo "ALL TESTS PASSED"
else
  echo "TESTS FAILED"
fi
exit "$fail"
