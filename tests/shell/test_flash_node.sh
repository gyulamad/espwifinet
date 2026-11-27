#!/usr/bin/env bash
# test_flash_node.sh — drives the REAL flash-node.sh with a stub arduino-cli
# (task #165): the plugged-in NodeMCU 1.0's row in `arduino-cli board list`
# reads type UNKNOWN with no FQBN at all, and the old column parser answered
# "no board found" to exactly that table while the node sat on /dev/ttyUSB0.
#   scenario 1: the tester's exact board list (tab-separated, Unknown type)
#               -> the port /dev/ttyUSB0 must be detected (the script then
#               stops on the missing device file in CI, which is the expected
#               boundary here — the point is it no longer says "no board");
#   scenario 2: the same table space-aligned (the other real rendering)
#               -> detected too;
#   scenario 3: a board list with NO serial port -> "no board" and nothing
#               may be compiled or uploaded;
#   scenario 4: --port given -> the full flow runs: compile with the esp8266
#               FQBN, upload to the given port, "flashed." printed;
#   scenario 5: --board esp32 -> the compile rides the ESP32 FQBN.
#   scenario 6: --board esp32 on the 1.0.x ESP32 core (task #171) -> refused:
#               that core's NVS caps blobs at 1984 bytes, below the node's
#               4096-byte EEPROM blob, so every save would fail; nothing may
#               be compiled;
#   scenario 7: --board esp32 on a 2.x+ core -> the flow runs.

set -u

command -v bash >/dev/null 2>&1 || { echo "SKIP: bash not available"; exit 0; }

repo="$(cd "$(dirname "$0")/../.." && pwd)"
tmp="$(mktemp -d)"
fail=0

cleanup() {
  chmod -R u+rwX "$tmp" 2>/dev/null
  rm -rf "$tmp"
}
trap cleanup EXIT

# an isolated repository root: flash-node.sh cd's to its own directory, so a
# copy of the script plus a dummy sketch keep the real tree untouched
mkdir -p "$tmp/node/arduino/ESP_node"
cp "$repo/flash-node.sh" "$tmp/node/flash-node.sh"
printf 'void setup() {}\nvoid loop() {}\n' > "$tmp/node/arduino/ESP_node/ESP_node.ino"

# the stub arduino-cli: board list answers from $STUB_BOARD_LIST, compile
# produces the binary the script checks for, every call lands in $STUB_LOG
mkdir -p "$tmp/bin"
cat > "$tmp/bin/arduino-cli" <<'STUB'
#!/usr/bin/env bash
cmd="$1"; shift
case "$cmd" in
  board)
    case "$STUB_BOARD_LIST" in
      unknown-tabs)
        printf 'Port\tProtocol\tType\tBoard Name\tFQBN\tCore\n'
        printf '/dev/ttyUSB0\tserial\tSerial Port (USB)\tUnknown\t\t\n'
        ;;
      unknown-spaces)
        printf 'Port         Protocol Type              Board Name FQBN Core\n'
        printf '/dev/ttyUSB0 serial   Serial Port (USB) Unknown\n'
        ;;
      empty)
        printf 'Port\tProtocol\tType\tBoard Name\tFQBN\tCore\n'
        ;;
    esac ;;
  compile)
    echo "compile $*" >> "$STUB_LOG"
    mkdir -p build && : > build/ESP_node.ino.bin ;;
  upload)
    echo "upload $*" >> "$STUB_LOG" ;;
  core)
    # task #171: flash-node.sh checks the installed esp32 core's version
    printf '%s\n' "${STUB_CORE_LIST:-}" ;;
esac
exit 0
STUB
chmod +x "$tmp/bin/arduino-cli"

run_flash() { # <board-list-fixture|-> [extra args...]
  local fixture="$1"; shift
  : > "$tmp/stub.log"
  STUB_BOARD_LIST="$fixture" STUB_LOG="$tmp/stub.log" STUB_CORE_LIST="${STUB_CORE_LIST:-}" \
    PATH="$tmp/bin:$PATH" bash "$tmp/node/flash-node.sh" "$@" > "$tmp/out.txt" 2> "$tmp/err.txt"
}

# ── scenario 1: the tester's exact Unknown-type table (tabs) ──
run_flash unknown-tabs
out="$(cat "$tmp/out.txt" "$tmp/err.txt")"
if printf '%s' "$out" | grep -q 'serial port /dev/ttyUSB0 does not exist' \
   && ! printf '%s' "$out" | grep -q 'no USB serial port'; then
  echo "[PASS] unknown-type board (tabs): /dev/ttyUSB0 detected from the board list"
else
  echo "[FAIL] unknown-type board (tabs): the plugged-in node was not detected; output:"
  printf '%s\n' "$out"
  fail=1
fi
[ -s "$tmp/stub.log" ] && { echo "[FAIL] unknown-type board (tabs): compile/upload ran despite the missing device"; cat "$tmp/stub.log"; fail=1; }

# ── scenario 2: the same table, space-aligned ──
run_flash unknown-spaces
out="$(cat "$tmp/out.txt" "$tmp/err.txt")"
if printf '%s' "$out" | grep -q 'serial port /dev/ttyUSB0 does not exist' \
   && ! printf '%s' "$out" | grep -q 'no USB serial port'; then
  echo "[PASS] unknown-type board (spaces): /dev/ttyUSB0 detected regardless of the column alignment"
else
  echo "[FAIL] unknown-type board (spaces): the plugged-in node was not detected; output:"
  printf '%s\n' "$out"
  fail=1
fi

# ── scenario 3: board list without any serial port ──
run_flash empty
out="$(cat "$tmp/out.txt" "$tmp/err.txt")"
if printf '%s' "$out" | grep -q 'no USB serial port in arduino-cli board list' \
   && [ ! -s "$tmp/stub.log" ]; then
  echo "[PASS] no serial port: refuses with the plug-it-in hint and compiles nothing"
else
  echo "[FAIL] no serial port: expected the no-board refusal and no compile/upload; output:"
  printf '%s\n' "$out"
  cat "$tmp/stub.log" 2>/dev/null
  fail=1
fi

# ── scenario 4: --port given -> the full compile+upload flow ──
# /dev/null exists everywhere, so the script's own existence check passes.
run_flash unknown-tabs --port /dev/null
rc=$?
out="$(cat "$tmp/out.txt" "$tmp/err.txt")"
if [ "$rc" -eq 0 ] \
   && grep -q 'compile --fqbn esp8266:esp8266:nodemcuv2 --output-dir ./build arduino/ESP_node' "$tmp/stub.log" \
   && grep -q 'upload --fqbn esp8266:esp8266:nodemcuv2 -p /dev/null --input-dir ./build arduino/ESP_node' "$tmp/stub.log" \
   && printf '%s' "$out" | grep -q 'flashed.'; then
  echo "[PASS] --port flow: compiled for esp8266:esp8266:nodemcuv2, uploaded to the given port"
else
  echo "[FAIL] --port flow: exit $rc, stub log:"; cat "$tmp/stub.log" 2>/dev/null; printf '%s\n' "$out"
  fail=1
fi

# ── scenario 5: --board esp32 rides the ESP32 FQBN ──
run_flash unknown-tabs --board esp32 --port /dev/null
rc=$?
if [ "$rc" -eq 0 ] && grep -q 'compile --fqbn esp32:esp32:esp32' "$tmp/stub.log"; then
  echo "[PASS] --board esp32: compiled for esp32:esp32:esp32"
else
  echo "[FAIL] --board esp32: exit $rc, stub log:"; cat "$tmp/stub.log" 2>/dev/null
  fail=1
fi

# ── scenario 6: --board esp32 on the 1.0.x core is refused (task #171) ──
STUB_CORE_LIST="esp32 1.0.6" run_flash unknown-tabs --board esp32 --port /dev/null
rc=$?
out="$(cat "$tmp/out.txt" "$tmp/err.txt")"
if [ "$rc" -eq 1 ] \
   && printf '%s' "$out" | grep -q "cannot hold the node's 4096-byte EEPROM blob" \
   && ! printf '%s' "$out" | grep -q 'flashed.' \
   && [ ! -s "$tmp/stub.log" ]; then
  echo "[PASS] esp32 core 1.0.x: refused with the NVS 1984-byte blob explanation, nothing compiled"
else
  echo "[FAIL] esp32 core 1.0.x: exit $rc, expected the core refusal and no compile; output:"
  printf '%s\n' "$out"; cat "$tmp/stub.log" 2>/dev/null
  fail=1
fi

# ── scenario 7: --board esp32 on a 2.x core runs the flow ──
STUB_CORE_LIST="esp32 2.0.17" run_flash unknown-tabs --board esp32 --port /dev/null
rc=$?
if [ "$rc" -eq 0 ] && grep -q 'compile --fqbn esp32:esp32:esp32' "$tmp/stub.log"; then
  echo "[PASS] esp32 core 2.x: compiled without the gate firing"
else
  echo "[FAIL] esp32 core 2.x: exit $rc, stub log:"; cat "$tmp/stub.log" 2>/dev/null
  fail=1
fi

if [ "$fail" -eq 0 ]; then
  echo "flash-node.sh port detection: ALL PASSED"
else
  echo "flash-node.sh port detection: FAILED"
fi
exit "$fail"
