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
#   --erase (task #173): the esptool binary/command spelling follows the
#   installed esptool, so no deprecation warning is ever provoked:
#   scenario 8:  esptool v5 -> `esptool ... erase-flash` (no .py, no underscore);
#   scenario 9:  py-only install, bundled v3 version line -> `esptool.py ... erase_flash`;
#   scenario 10: the v4 boundary -> erase-flash;
#   scenario 11: unparseable version line -> erase_flash (widest-compatible);
#   scenario 12: no esptool at all -> refused with the pip hint, nothing compiled.
#   verified upload (task #174): the esp8266 core's upload wrapper prints
#   esptool's fatal error and still exits 0:
#   scenario 13: fatal on every attempt (esp8266 wording) -> FAILS, never "flashed.";
#   scenario 14: the same with the v4+/esp32 wording;
#   scenario 15: fatal once, then a clean upload -> retried and flashed;
#   scenario 16: arduino-cli's own non-zero exit -> dies at once, no retry.
#   flash layout follows the chip (follow-up of #173/#174): a 1MB chip cannot hold the
#   4MB NodeMCU layout's settings sector, every setup-page save failed:
#   scenario 17: 1MB chip (esptool v5) -> built as generic:eesz=1M, both the
#                compile and the upload ride it;
#   scenario 18: 2MB chip on a py-only v3 install -> eesz=2M, `esptool.py ... flash_id`;
#   scenario 19: 4MB chip -> the NodeMCU default stays;
#   scenario 20: no esptool -> default layout WITH a warning naming the override;
#   scenario 21: unreadable / unknown size -> default layout with a warning;
#   scenario 22: FQBN_ESP8266 set by the caller wins, the chip is never asked;
#   scenario 23: --board esp32 never asks esptool for a flash size;
#   scenario 24: UPLOAD_ATTEMPTS overrides the default of 5.

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
    echo "upload $*" >> "$STUB_LOG"
    n=$(( $(cat "$STUB_COUNT" 2>/dev/null || echo 0) + 1 )); echo "$n" > "$STUB_COUNT"
    # task #174: the esp8266 core's upload.py prints the fatal and exits 0
    case "${STUB_UPLOAD:-ok}" in
      fatal-esp8266) echo 'A fatal esptool.py error occurred: Invalid head of packet (0x46)' >&2 ;;
      fatal-esp32)   echo 'A fatal error occurred: Failed to connect to ESP32' >&2 ;;
      fatal-once)    [ "$n" -eq 1 ] && echo 'A fatal esptool.py error occurred: Invalid head of packet (0x46)' >&2 ;;
      fail)          echo 'Error during upload: uploading error' >&2; exit 1 ;;
    esac ;;
  core)
    # task #171: flash-node.sh checks the installed esp32 core's version
    printf '%s\n' "${STUB_CORE_LIST:-}" ;;
esac
exit 0
STUB
chmod +x "$tmp/bin/arduino-cli"

# esptool stubs (task #173): the version line comes from $STUB_ESPTOOL_VERSION,
# every other call is logged verbatim under the name it was invoked by
mkdir -p "$tmp/bin-modern" "$tmp/bin-py" "$tmp/sysbin"
cat > "$tmp/bin-modern/esptool" <<'STUB'
#!/usr/bin/env bash
if [ "${1:-}" = version ]; then printf '%s\n' "${STUB_ESPTOOL_VERSION:-}"; exit 0; fi
echo "$(basename "$0") $*" >> "$STUB_LOG"
case "${3:-}" in
  flash-id|flash_id)
    [ -n "${STUB_FLASH_SIZE:-}" ] && printf 'Manufacturer: 68\nDevice: 4014\nDetected flash size: %s\n' "$STUB_FLASH_SIZE"
    exit 0 ;;
esac
STUB
chmod +x "$tmp/bin-modern/esptool"
cp "$tmp/bin-modern/esptool" "$tmp/bin-modern/esptool.py"
cp "$tmp/bin-modern/esptool" "$tmp/bin-py/esptool.py"

# a PATH of its own: the host's real esptool (or any other) must never leak
# into a scenario - the suite tests the stubs, not the host toolchain
for t in env bash sed grep awk tee wc tr head cat mktemp rm sleep dirname basename mkdir; do
  src="$(command -v "$t" 2>/dev/null)" && ln -sf "$src" "$tmp/sysbin/$t"
done
HERMETIC_BASE="$tmp/bin:$tmp/sysbin"

run_flash() { # <board-list-fixture|-> [extra args...]
  local fixture="$1"; shift
  : > "$tmp/stub.log"; rm -f "$tmp/upload.count"
  STUB_BOARD_LIST="$fixture" STUB_LOG="$tmp/stub.log" STUB_CORE_LIST="${STUB_CORE_LIST:-}" \
    STUB_COUNT="$tmp/upload.count" UPLOAD_RETRY_DELAY=0 \
    PATH="${STUB_BIN_PATH:-$HERMETIC_BASE}" bash "$tmp/node/flash-node.sh" "$@" > "$tmp/out.txt" 2> "$tmp/err.txt"
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

# ── scenarios 8-12: --erase picks the tool and command spelling (task #173) ──
erase_case() { # <label> <bin-dir> <version line> <expected first stub-log line>
  local label="$1" bindir="$2" ver="$3" want="$4" first
  FQBN_ESP8266=esp8266:esp8266:nodemcuv2 STUB_BIN_PATH="$bindir:$HERMETIC_BASE" STUB_ESPTOOL_VERSION="$ver" \
    run_flash unknown-tabs --port /dev/null --erase
  rc=$?
  first="$(sed -n 1p "$tmp/stub.log")"
  if [ "$rc" -eq 0 ] && [ "$first" = "$want" ] \
     && [ "$(grep -c '^esptool' "$tmp/stub.log")" -eq 1 ] \
     && grep -q '^compile ' "$tmp/stub.log" && grep -q '^upload ' "$tmp/stub.log"; then
    echo "[PASS] --erase $label: '$want', then compile + upload"
  else
    echo "[FAIL] --erase $label: exit $rc, expected first call '$want'; stub log:"
    cat "$tmp/stub.log"; cat "$tmp/err.txt"
    fail=1
  fi
}
erase_case "esptool v5" "$tmp/bin-modern" "esptool v5.4.0" "esptool --port /dev/null erase-flash"
erase_case "py-only install, bundled v3" "$tmp/bin-py" "esptool.py v3.0" "esptool.py --port /dev/null erase_flash"
erase_case "v4 boundary" "$tmp/bin-modern" "esptool.py v4.0" "esptool --port /dev/null erase-flash"
erase_case "unparseable version line" "$tmp/bin-modern" "no idea what this is" "esptool --port /dev/null erase_flash"

STUB_BIN_PATH="$HERMETIC_BASE" run_flash unknown-tabs --port /dev/null --erase
rc=$?
out="$(cat "$tmp/out.txt" "$tmp/err.txt")"
if [ "$rc" -eq 1 ] && printf '%s' "$out" | grep -q -- '--erase needs esptool (pip install esptool)' \
   && [ ! -s "$tmp/stub.log" ]; then
  echo "[PASS] --erase without esptool: refused with the pip hint, nothing compiled"
else
  echo "[FAIL] --erase without esptool: exit $rc; output:"; printf '%s\n' "$out"; cat "$tmp/stub.log"
  fail=1
fi

# ── scenarios 13-16: the upload is verified, not trusted (task #174) ──
upload_case() { # <label> <STUB_UPLOAD> <expected rc> <expected upload count> <must match> <board args...>
  local label="$1" mode="$2" wantrc="$3" wantn="$4" must="$5"; shift 5
  STUB_UPLOAD="$mode" run_flash unknown-tabs --port /dev/null "$@"
  rc=$?
  local out n ok=1
  out="$(cat "$tmp/out.txt" "$tmp/err.txt")"
  n="$(grep -c '^upload ' "$tmp/stub.log")"
  [ "$rc" -eq "$wantrc" ] || ok=0
  [ "$n" -eq "$wantn" ] || ok=0
  printf '%s' "$out" | grep -q -- "$must" || ok=0
  if [ "$wantrc" -ne 0 ]; then
    printf '%s' "$out" | grep -q 'flashed. next:' && ok=0
  else
    printf '%s' "$out" | grep -q 'flashed. next:' || ok=0
  fi
  if [ "$ok" -eq 1 ]; then
    echo "[PASS] $label: exit $rc after $n upload attempt(s)"
  else
    echo "[FAIL] $label: exit $rc (want $wantrc), $n attempt(s) (want $wantn), expected '$must'; output:"
    printf '%s\n' "$out"
    fail=1
  fi
}
upload_case "swallowed esptool fatal (esp8266 wording) on every attempt" fatal-esp8266 1 5 'upload did NOT stick'
upload_case "swallowed esptool fatal (esp32 wording) on every attempt" fatal-esp32 1 5 'upload did NOT stick' --board esp32
upload_case "fatal once, then a clean upload: retried and flashed" fatal-once 0 2 'retrying'
upload_case "arduino-cli's own failure: dies at once, no retry" fail 1 1 'upload failed'

# ── scenarios 17-23: the flash layout follows the chip (follow-up of #173/#174) ──
layout_case() { # <label> <bin-dir> <version> <flash size> <expected fqbn> <expected flash-id call|-> [extra args...]
  local label="$1" bindir="$2" ver="$3" size="$4" want="$5" idcall="$6"; shift 6
  STUB_BIN_PATH="$bindir:$HERMETIC_BASE" STUB_ESPTOOL_VERSION="$ver" STUB_FLASH_SIZE="$size" \
    run_flash unknown-tabs --port /dev/null "$@"
  rc=$?
  local ok=1
  [ "$rc" -eq 0 ] || ok=0
  grep -q "^compile --fqbn $want --output-dir" "$tmp/stub.log" || ok=0
  grep -q "^upload --fqbn $want -p /dev/null" "$tmp/stub.log" || ok=0
  if [ "$idcall" != "-" ]; then grep -qx "$idcall" "$tmp/stub.log" || ok=0; fi
  if [ "$ok" -eq 1 ]; then
    echo "[PASS] $label: built as $want"
  else
    echo "[FAIL] $label: exit $rc, expected $want (flash-id call: $idcall); stub log:"
    cat "$tmp/stub.log"; cat "$tmp/out.txt" "$tmp/err.txt"
    fail=1
  fi
}
layout_case "1MB chip, esptool v5" "$tmp/bin-modern" "esptool v5.4.0" 1MB \
  "esp8266:esp8266:generic:eesz=1M" "esptool --port /dev/null flash-id"
layout_case "2MB chip, py-only v3" "$tmp/bin-py" "esptool.py v3.0" 2MB \
  "esp8266:esp8266:generic:eesz=2M" "esptool.py --port /dev/null flash_id"
layout_case "4MB chip keeps the NodeMCU default" "$tmp/bin-modern" "esptool v5.4.0" 4MB \
  "esp8266:esp8266:nodemcuv2" "esptool --port /dev/null flash-id"

STUB_BIN_PATH="$HERMETIC_BASE" run_flash unknown-tabs --port /dev/null
rc=$?
out="$(cat "$tmp/out.txt" "$tmp/err.txt")"
if [ "$rc" -eq 0 ] && grep -q '^compile --fqbn esp8266:esp8266:nodemcuv2 ' "$tmp/stub.log" \
   && printf '%s' "$out" | grep -q 'could not read the chip.s flash size' \
   && printf '%s' "$out" | grep -q 'FQBN_ESP8266=esp8266:esp8266:generic:eesz=1M'; then
  echo "[PASS] no esptool: default layout, with the warning that names the 1MB override"
else
  echo "[FAIL] no esptool: exit $rc; output:"; printf '%s\n' "$out"; cat "$tmp/stub.log"
  fail=1
fi

STUB_BIN_PATH="$tmp/bin-modern:$HERMETIC_BASE" STUB_ESPTOOL_VERSION="esptool v5.4.0" STUB_FLASH_SIZE="" \
  run_flash unknown-tabs --port /dev/null
rc=$?
out="$(cat "$tmp/out.txt" "$tmp/err.txt")"
if [ "$rc" -eq 0 ] && grep -q '^compile --fqbn esp8266:esp8266:nodemcuv2 ' "$tmp/stub.log" \
   && printf '%s' "$out" | grep -q 'could not read the chip.s flash size'; then
  echo "[PASS] unreadable flash size: default layout with the warning"
else
  echo "[FAIL] unreadable flash size: exit $rc; output:"; printf '%s\n' "$out"; cat "$tmp/stub.log"
  fail=1
fi
STUB_BIN_PATH="$tmp/bin-modern:$HERMETIC_BASE" STUB_ESPTOOL_VERSION="esptool v5.4.0" STUB_FLASH_SIZE="3MB" \
  run_flash unknown-tabs --port /dev/null
rc=$?
out="$(cat "$tmp/out.txt" "$tmp/err.txt")"
if [ "$rc" -eq 0 ] && grep -q '^compile --fqbn esp8266:esp8266:nodemcuv2 ' "$tmp/stub.log" \
   && printf '%s' "$out" | grep -q "flash size '3MB' is not one this script knows"; then
  echo "[PASS] unknown flash size: default layout with the warning"
else
  echo "[FAIL] unknown flash size: exit $rc; output:"; printf '%s\n' "$out"; cat "$tmp/stub.log"
  fail=1
fi

FQBN_ESP8266="esp8266:esp8266:d1_mini" STUB_BIN_PATH="$tmp/bin-modern:$HERMETIC_BASE" \
  STUB_ESPTOOL_VERSION="esptool v5.4.0" STUB_FLASH_SIZE="1MB" \
  run_flash unknown-tabs --port /dev/null
rc=$?
if [ "$rc" -eq 0 ] && grep -q '^compile --fqbn esp8266:esp8266:d1_mini ' "$tmp/stub.log" \
   && ! grep -q 'flash.id' "$tmp/stub.log"; then
  echo "[PASS] FQBN_ESP8266 set by the caller wins: the chip is never asked"
else
  echo "[FAIL] explicit FQBN: exit $rc; stub log:"; cat "$tmp/stub.log"
  fail=1
fi

STUB_BIN_PATH="$tmp/bin-modern:$HERMETIC_BASE" STUB_ESPTOOL_VERSION="esptool v5.4.0" STUB_FLASH_SIZE="1MB" \
  run_flash unknown-tabs --port /dev/null --board esp32
rc=$?
if [ "$rc" -eq 0 ] && grep -q '^compile --fqbn esp32:esp32:esp32 ' "$tmp/stub.log" \
   && ! grep -q '^esptool' "$tmp/stub.log"; then
  echo "[PASS] --board esp32: esptool is never asked for a flash size"
else
  echo "[FAIL] esp32 layout: exit $rc; stub log:"; cat "$tmp/stub.log"
  fail=1
fi

# ── scenario 24: UPLOAD_ATTEMPTS overrides the default of 5 ──
UPLOAD_ATTEMPTS=2 upload_case "UPLOAD_ATTEMPTS=2 caps the retries" fatal-esp8266 1 2 'upload did NOT stick'

if [ "$fail" -eq 0 ]; then
  echo "flash-node.sh port detection: ALL PASSED"
else
  echo "flash-node.sh port detection: FAILED"
fi
exit "$fail"
