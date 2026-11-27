#!/usr/bin/env bash
# run_tests.sh — the full test suite for the ESP Wifi Network base project.
#   1. node unit tests (relay logic)
#   2. node e2e test (spawns the real relay, drives it over TCP + HTTP)
#   3. discovery e2e test (#160: the real relay answers UDP discovery probes;
#      the firmware's real search code finds it over real sockets)
#   4. C++ logic tests (node firmware's net/log/ota logic against stubs, g++)
#   5. discovery logic + glue tests (#160, same two-board build)
#   6. firmware stack budget (frame sizes, tests/stack_budget.sh, g++)
#   7. flash-node.sh port detection (#165: stub arduino-cli, unknown-board tables)
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

# #160: the discovery responder has its own e2e — the real relay answers on
# UDP and the firmware's real search code (relay_find_logic.h, compiled into
# tests/cpp/relay_find_tool.cpp) finds it over real sockets.
(cd nodejs/esp-relay && node tests/e2e/test_discovery_e2e.js) || fail=1

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
  # #160: the discovery logic + its Arduino glue, same two-board build.
  echo ""
  echo "── discovery tests (ESP8266 branch) ──"
  tmpbin="$(mktemp -d)/test_relay_find_esp8266"
  if g++ -std=gnu++17 -Wall -Wextra -DESP8266 -Itests/cpp/stubs \
      tests/cpp/test_relay_find.cpp -o "$tmpbin"; then
    "$tmpbin" || fail=1
  else
    echo "FAIL discovery build (esp8266)"
    fail=1
  fi
  echo ""
  echo "── discovery tests (ESP32 branch) ──"
  tmpbin32="$(mktemp -d)/test_relay_find_esp32"
  if g++ -std=gnu++17 -Wall -Wextra -DESP32 -Itests/cpp/stubs/esp32 -Itests/cpp/stubs \
      tests/cpp/test_relay_find.cpp -o "$tmpbin32"; then
    "$tmpbin32" || fail=1
  else
    echo "FAIL discovery build (esp32)"
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

# #159: the installers and the flash/OTA automation are shell scripts - a
# syntax check catches the broken-one-liner class of failure in CI.
echo ""
echo "── install/flash/ota scripts: bash -n (#159) ──"
for sh in install-relay.sh install-dash.sh flash-node.sh ota-deploy.sh; do
  bash -n "$sh" || fail=1
done

# #165: flash-node.sh's port detection is a text-parse step that must cope
# with the plugged-in node board list CANNOT identify (type Unknown, no
# FQBN) — the tester's NodeMCU 1.0 sat on /dev/ttyUSB0 while the script
# answered "no board found". tests/shell/test_flash_node.sh drives the real
# script against a stub arduino-cli serving exactly those tables.
echo ""
echo "── flash-node.sh port detection (#165) ──"
bash tests/shell/test_flash_node.sh || fail=1

# #171: the setup form's browser script (Show/Hide toggles, row add/remove)
# is embedded in setup_logic.h as a raw string and served as /setup.js — a
# syntax error there would disable every button on the page while the C++
# suite stays green. The step parses the extracted script with node.
echo ""
echo "── setup.js syntax (extracted from setup_logic.h, #171) ──"
if command -v node >/dev/null 2>&1; then
  node -e '
    const fs = require("fs");
    const h = fs.readFileSync("arduino/ESP_node/setup_logic.h", "utf8");
    const m = h.match(/R"js\(([\s\S]*?)\)js"/);
    if (!m) { console.error("FAIL: the setup form script was not found in setup_logic.h"); process.exit(1); }
    new Function(m[1]);
    console.log("setup.js parses clean (" + m[1].length + " bytes)");
  ' || fail=1
else
  echo "node not found, skipping"
fi

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

# Task #165: the real-board compile gate. The desktop C++ tests build the sketch headers against
# stubs, and a stub can mirror a name the real core never had — #165's second NodeMCU 1.0 error
# ('Updater' was not declared; the core names its flasher object Update) was exactly that blind
# spot. When arduino-cli with the two cores is installed, the sketch is compiled for BOTH boards
# the way the Arduino IDE compiles it, so a wrong core API or a wrong object name fails here and
# not on the workbench. Like the php block above, a machine without the toolchain skips the gate
# (install: arduino-cli core install esp32:esp32 esp8266:esp8266).
echo ""
echo "── real-board compile gate: arduino-cli, both boards (#165) ──"
if command -v arduino-cli >/dev/null 2>&1; then
  mkdir -p build/arduino
  for pair in "${FQBN_ESP32:-esp32:esp32:esp32}" "${FQBN_ESP8266:-esp8266:esp8266:nodemcuv2}"; do
    if arduino-cli compile --fqbn "$pair" --output-dir "build/arduino/${pair//:/_}" --warnings default arduino/ESP_node > "build/arduino/${pair//:/_}.log" 2>&1; then
      echo "[PASS] arduino/ESP_node compiles for $pair"
    else
      echo "[FAIL] arduino/ESP_node does not compile for $pair (last 15 lines of the log):"
      tail -15 "build/arduino/${pair//:/_}.log"
      fail=1
    fi
  done
else
  echo "real-board compile skipped (no arduino-cli; the desktop stubs are not the real cores — install it so wrong core APIs fail here)"
fi

echo ""
if [ "$fail" -eq 0 ]; then
  echo "ALL TESTS PASSED"
else
  echo "TESTS FAILED"
fi
exit "$fail"
