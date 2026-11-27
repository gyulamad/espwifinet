#!/usr/bin/env bash
# flash-node.sh — automates section 3 of INSTALL.md (the ONE wired flash a
# node ever needs, task #159). Run from the repository root:
#   ./flash-node.sh                          # esp8266 on the first USB port
#   ./flash-node.sh --board esp32            # an ESP32/WROOM32 board
#   ./flash-node.sh --port /dev/ttyACM0      # pick the port yourself
#   ./flash-node.sh --erase                  # factory reset BEFORE the upload
#
# The port is auto-detected from `arduino-cli board list`; a port whose board
# reads as type UNKNOWN (most clone boards — the list row has no FQBN) is
# used all the same, because the FQBN for the upload comes from --board, not
# from the USB identification (task #165).
#
# It compiles arduino/ESP_node for the board and uploads it with arduino-cli
# (the same two commands the manual section shows). The compiled image is the
# same one the dashboard serves for OTA, so what you flash by cable and what
# the fleet pulls over the air stay identical.
#
# There is NOTHING to edit first: the sketch carries no config file — the
# node's WiFi list and its relay address + fleet key are typed on the node's
# own setup WiFi page after this flash (INSTALL.md section 4).
#
# --erase runs `esptool --port <port> erase-flash` BEFORE the upload: the
# flash store survives ordinary reflashes BY DESIGN, so a factory reset is a
# deliberate step (it clears the WiFi list, the relay address + fleet key and
# both life-cycle flags; the dashboard's history for the device id stays).
# The script prefers the modern `esptool` binary (esptool.py is deprecated
# since v5) and spells the command the way the installed version wants it
# (erase-flash from v4 on, erase_flash for v3 / unknown), so no deprecation
# warning is printed (task #173).
#
# The flash LAYOUT follows the chip (follow-up of #173/#174): the
# default NodeMCU build assumes a 4MB chip and keeps the node's settings
# sector 4MB into the flash. A 1MB or 2MB chip has no such sector, so every
# setup-page save fails ("the flash write did not persist") - the tester's
# 1MB board did exactly that. For esp8266 the script therefore reads the
# chip's flash size (`esptool flash-id`) and builds the matching layout of the
# esp8266 core's generic board (1MB -> esp8266:esp8266:generic:eesz=1M); 4MB
# and bigger keep the NodeMCU default. Needs esptool on the PATH (pip install
# esptool); without it the default is used with a warning. An FQBN_ESP8266
# you set yourself always wins and skips the detection.
#
# The upload is VERIFIED (task #174): the esp8266 core's upload wrapper prints
# esptool's fatal error and still exits 0, so the toolchain output is scanned
# for it. A failed write is retried (the first upload right after an erase is
# the fragile one) and, if it never sticks, the script FAILS instead of
# printing "flashed." for a half-written node.
#
# Settings (environment variables):
#   FQBN_ESP8266 / FQBN_ESP32   override the board identifiers
#   UPLOAD_ATTEMPTS             upload tries before giving up (default 5)
#   UPLOAD_RETRY_DELAY          seconds between tries (default 2)
#
# Exit code: 0 flashed, 1 failed, 2 wrong usage.
set -u

cd "$(dirname "$0")" || exit 1

FQBN_ESP8266_EXPLICIT="${FQBN_ESP8266:+1}"   # set by the caller: skip the flash-size detection
FQBN_ESP8266="${FQBN_ESP8266:-esp8266:esp8266:nodemcuv2}"
FQBN_ESP8266_GENERIC="esp8266:esp8266:generic"   # the board whose menu has the 1MB/2MB layouts
FQBN_ESP32="${FQBN_ESP32:-esp32:esp32:esp32}"
BUILD_DIR="./build"
UPLOAD_ATTEMPTS="${UPLOAD_ATTEMPTS:-5}"
UPLOAD_RETRY_DELAY="${UPLOAD_RETRY_DELAY:-2}"
# esptool's own fatal line: 'A fatal esptool.py error occurred' (esptool 3.x,
# the esp8266 core's bundled one) / 'A fatal error occurred' (v4+, esp32 path)
ESPTOOL_FATAL_RE='A fatal (esptool\.py )?error occurred'

usage() { sed -n '2,/^set -u/p' "${BASH_SOURCE[0]}" | sed '$d' | sed 's/^# \{0,1\}//'; }

BOARD="esp8266"
PORT=""
ERASE=0
while [ $# -gt 0 ]; do
  case "$1" in
    --board) [ $# -ge 2 ] || { echo "--board needs esp8266 or esp32" >&2; exit 2; }; BOARD="$2"; shift 2 ;;
    --board=*) BOARD="${1#--board=}"; shift ;;
    --port) [ $# -ge 2 ] || { echo "--port needs a device path" >&2; exit 2; }; PORT="$2"; shift 2 ;;
    --port=*) PORT="${1#--port=}"; shift ;;
    --erase) ERASE=1; shift ;;
    -h|--help) usage; exit 0 ;;
    *) echo "flash-node: unknown argument: $1 (try --help)" >&2; exit 2 ;;
  esac
done

say() { echo "flash-node: $*"; }
die() { echo "flash-node: FAILED: $*" >&2; exit 1; }

case "$UPLOAD_ATTEMPTS" in
  ''|*[!0-9]*|0) echo "flash-node: UPLOAD_ATTEMPTS must be a positive number (got: $UPLOAD_ATTEMPTS)" >&2; exit 2 ;;
esac
case "$UPLOAD_RETRY_DELAY" in
  ''|*[!0-9]*) echo "flash-node: UPLOAD_RETRY_DELAY must be whole seconds (got: $UPLOAD_RETRY_DELAY)" >&2; exit 2 ;;
esac

case "$BOARD" in
  esp8266) FQBN="$FQBN_ESP8266" ;;
  esp32)   FQBN="$FQBN_ESP32" ;;
  *) echo "flash-node: --board must be esp8266 or esp32 (got: $BOARD)" >&2; exit 2 ;;
esac

command -v arduino-cli >/dev/null 2>&1 || die "arduino-cli not found - install it (INSTALL.md section 3 shows the IDE alternative)"
[ -f arduino/ESP_node/ESP_node.ino ] || die "run this from the repository root (arduino/ESP_node/ESP_node.ino not found)"

# ── the ESP32 core must carry multi-page NVS blobs (task #171) ───────────
# The node's whole EEPROM space is ONE 4096-byte blob (the WiFi region and
# the relay configuration share it). The ESP32 EEPROM library stores that as
# an NVS blob, and NVS caps a blob at 1984 bytes on the cores without
# multi-page blob support (the arduino-esp32 1.0.x line, ESP-IDF 3.x):
# EEPROM.begin() then fails its own space check and EVERY save afterwards
# answers "Could not write the credentials to flash" — the node can never
# leave the setup page. Refuse to build for esp32 on such a core instead of
# flashing an uninstallable node; the esp8266 core has no such limit (a real
# flash sector there).
if [ "$BOARD" = esp32 ]; then
  CORE_ESP32="$(arduino-cli core list 2>/dev/null | awk '$1 == "esp32" {print $2}')"
  case "$CORE_ESP32" in
    0.*|1.*) die "the installed esp32 core ('$CORE_ESP32') cannot hold the node's 4096-byte EEPROM blob (its NVS caps blobs at 1984 bytes) - upgrade: arduino-cli core update-index && arduino-cli core install esp32:esp32 (2.0.0 or newer)" ;;
  esac
fi

# ── esptool helpers (tasks #173, #174 follow-up) ───────────────────────────────────
# the modern binary name first (esptool.py is deprecated since v5; every pip
# install ships both entry points), the old one as the fallback
find_esptool() {
  command -v esptool >/dev/null 2>&1 && { echo esptool; return 0; }
  command -v esptool.py >/dev/null 2>&1 && { echo esptool.py; return 0; }
  return 1
}

# the tool's major version from the first line of `<tool> version`
# ('esptool v5.4.0' / 'esptool.py v3.0'); empty when it cannot be read
esptool_major() {
  "$1" version 2>/dev/null | head -n 1 | sed -n 's/^[^0-9]*\([0-9][0-9]*\)\..*/\1/p'
}

# v4+ spells its commands with dashes (the underscore form only warns there);
# v3 and an unreadable version keep the underscore spelling, which every
# version accepts. usage: esptool_command <tool> erase flash -> erase-flash
esptool_command() {
  local tool="$1" major; shift
  major="$(esptool_major "$tool")"
  case "$major" in
    ''|*[!0-9]*) ( IFS=_; echo "$*" ); return ;;
  esac
  [ "$major" -ge 4 ] || { ( IFS=_; echo "$*" ); return; }
  ( IFS=-; echo "$*" )
}

# the chip's flash size as esptool words ("1MB"), empty when unreadable
detect_flash_size() { # <port>
  local tool cmd
  tool="$(find_esptool)" || return 1
  cmd="$(esptool_command "$tool" flash id)"
  "$tool" --port "$1" "$cmd" 2>&1 | sed -n 's/^Detected flash size: *//p' | head -n 1 | tr -d '\r '
}

# esptool's size words -> the esp8266 core's eesz menu value (generic board)
flash_eesz() {
  case "$1" in
    512KB) echo 512K ;;
    1MB)   echo 1M ;;
    2MB)   echo 2M ;;
    *)     return 1 ;;
  esac
}

# ── the esp8266 flash layout follows the chip (follow-up of #173/#174) ─────────────────
select_esp8266_layout() {
  local size eesz
  size="$(detect_flash_size "$PORT")"
  if [ -z "$size" ]; then
    say "WARNING: could not read the chip's flash size (esptool missing? pip install esptool) -"
    say "building the default 4MB NodeMCU layout. A 1MB/2MB chip cannot save the setup page"
    say "with it; if the save fails, re-run with FQBN_ESP8266=$FQBN_ESP8266_GENERIC:eesz=1M"
    return 0
  fi
  case "$size" in
    4MB|8MB|16MB) say "flash: $size - the default layout fits"; return 0 ;;
  esac
  eesz="$(flash_eesz "$size")" || {
    say "WARNING: flash size '$size' is not one this script knows - building the default layout"
    return 0
  }
  FQBN="$FQBN_ESP8266_GENERIC:eesz=$eesz"
  say "flash: $size chip - building its own layout ($FQBN) so the settings sector lies inside the chip"
}

# ── the port ─────────────────────────────────────────────────────────────
if [ -z "$PORT" ]; then
  say "no --port given: looking for a board with arduino-cli board list"
  # task #165: cut the port out by its own /dev/... token instead of parsing
  # columns — board list renders tab-separated in one version and space-
  # aligned in another, and the plugged-in NodeMCU's row reads "Unknown" with
  # no FQBN at all (which the column parse treated as no board at all).
  PORT="$(arduino-cli board list 2>/dev/null | grep -oE '/dev/(ttyUSB|ttyACM|cu\.usb)[0-9A-Za-z._-]*' | head -n 1)"
  [ -n "$PORT" ] || die "no USB serial port in arduino-cli board list - plug the node in, or pass --port /dev/ttyUSB0 (Windows: COM3-style)"
fi
[ -e "$PORT" ] || die "serial port $PORT does not exist (arduino-cli board list shows the ports)"
if [ "$BOARD" = esp8266 ] && [ -z "$FQBN_ESP8266_EXPLICIT" ]; then
  select_esp8266_layout
fi
say "board: $BOARD ($FQBN), port: $PORT"

# ── optional factory reset ───────────────────────────────────────────────
if [ "$ERASE" = 1 ]; then
  ESPTOOL="$(find_esptool)" || die "--erase needs esptool (pip install esptool)"
  ERASE_CMD="$(esptool_command "$ESPTOOL" erase flash)"
  say "ERASING the flash on $PORT - the node forgets its WiFi list, relay address +"
  say "fleet key and both life-cycle flags; the dashboard's history for the device stays"
  "$ESPTOOL" --port "$PORT" "$ERASE_CMD" || die "$ERASE_CMD failed"
fi

# ── compile + upload ─────────────────────────────────────────────────────
say "compiling for $FQBN"
arduino-cli compile --fqbn "$FQBN" --output-dir "$BUILD_DIR" arduino/ESP_node || die "compile failed"
BIN="$BUILD_DIR/ESP_node.ino.bin"
[ -f "$BIN" ] || die "the build did not produce $BIN"
say "uploading $(wc -c < "$BIN" | tr -d ' ') bytes to $PORT"
# ── verified upload (task #174) ──────────────────────────────────────────
# one attempt, its toolchain output kept in $UPLOAD_LOG while it still reaches
# the terminal. Returns 0 = stuck, 1 = arduino-cli itself failed (exit status),
# 2 = esptool's fatal error is in the output although the exit status said 0
# (the esp8266 core's upload wrapper swallows it).
upload_once() {
  local rc
  arduino-cli upload --fqbn "$FQBN" -p "$PORT" --input-dir "$BUILD_DIR" arduino/ESP_node 2>&1 | tee "$UPLOAD_LOG"
  rc="${PIPESTATUS[0]}"
  [ "$rc" -eq 0 ] || return 1
  grep -Eq "$ESPTOOL_FATAL_RE" "$UPLOAD_LOG" && return 2
  return 0
}

UPLOAD_LOG="$(mktemp "${TMPDIR:-/tmp}/flash-node-upload.XXXXXX")" || die "cannot create a temporary file for the upload output"
attempt=1
while :; do
  upload_once
  status=$?
  [ "$status" -eq 0 ] && break
  [ "$status" -eq 1 ] && die "upload failed (arduino-cli's own failure; its output is kept in $UPLOAD_LOG)"
  [ "$attempt" -lt "$UPLOAD_ATTEMPTS" ] || die "the upload did NOT stick: esptool reported a fatal error on all $UPLOAD_ATTEMPTS attempts although arduino-cli exited 0 (the esp8266 core's upload wrapper swallows it). The node is NOT flashed (empty after --erase) - check the cable/port and re-run the same command; the erase need not be repeated. Toolchain output: $UPLOAD_LOG"
  say "esptool aborted mid-upload (attempt $attempt of $UPLOAD_ATTEMPTS) - the flash is not written, retrying"
  attempt=$((attempt + 1))
  sleep "$UPLOAD_RETRY_DELAY"
done
rm -f "$UPLOAD_LOG"

echo
say "flashed. next: power the node, join its WiFi 'espnode-setup-xxxxxx'"
say "(password espnode1234 unless overridden) and fill the setup page -"
say "INSTALL.md section 4 walks it. Open the serial monitor (115200) to watch"
say "the boot: arduino-cli monitor -p $PORT -b $FQBN --config baudrate=115200"
exit 0
