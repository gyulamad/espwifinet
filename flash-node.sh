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
# --erase runs `esptool.py --port <port> erase_flash` BEFORE the upload: the
# flash store survives ordinary reflashes BY DESIGN, so a factory reset is a
# deliberate step (it clears the WiFi list, the relay address + fleet key and
# both life-cycle flags; the dashboard's history for the device id stays).
#
# Settings (environment variables):
#   FQBN_ESP8266 / FQBN_ESP32   override the board identifiers
#
# Exit code: 0 flashed, 1 failed, 2 wrong usage.
set -u

cd "$(dirname "$0")" || exit 1

FQBN_ESP8266="${FQBN_ESP8266:-esp8266:esp8266:nodemcuv2}"
FQBN_ESP32="${FQBN_ESP32:-esp32:esp32:esp32}"
BUILD_DIR="./build"

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

case "$BOARD" in
  esp8266) FQBN="$FQBN_ESP8266" ;;
  esp32)   FQBN="$FQBN_ESP32" ;;
  *) echo "flash-node: --board must be esp8266 or esp32 (got: $BOARD)" >&2; exit 2 ;;
esac

command -v arduino-cli >/dev/null 2>&1 || die "arduino-cli not found - install it (INSTALL.md section 3 shows the IDE alternative)"
[ -f arduino/ESP_node/ESP_node.ino ] || die "run this from the repository root (arduino/ESP_node/ESP_node.ino not found)"

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
say "board: $BOARD ($FQBN), port: $PORT"

# ── optional factory reset ───────────────────────────────────────────────
if [ "$ERASE" = 1 ]; then
  command -v esptool.py >/dev/null 2>&1 || command -v esptool >/dev/null 2>&1 || die "--erase needs esptool.py (pip install esptool)"
  say "ERASING the flash on $PORT - the node forgets its WiFi list, relay address +"
  say "fleet key and both life-cycle flags; the dashboard's history for the device stays"
  if command -v esptool.py >/dev/null 2>&1; then
    esptool.py --port "$PORT" erase_flash || die "erase_flash failed"
  else
    esptool --port "$PORT" erase_flash || die "erase_flash failed"
  fi
fi

# ── compile + upload ─────────────────────────────────────────────────────
say "compiling for $FQBN"
arduino-cli compile --fqbn "$FQBN" --output-dir "$BUILD_DIR" arduino/ESP_node || die "compile failed"
BIN="$BUILD_DIR/ESP_node.ino.bin"
[ -f "$BIN" ] || die "the build did not produce $BIN"
say "uploading $(wc -c < "$BIN" | tr -d ' ') bytes to $PORT"
arduino-cli upload --fqbn "$FQBN" -p "$PORT" --input-dir "$BUILD_DIR" arduino/ESP_node || die "upload failed"

echo
say "flashed. next: power the node, join its WiFi 'espnode-setup-xxxxxx'"
say "(password espnode1234 unless overridden) and fill the setup page -"
say "INSTALL.md section 4 walks it. Open the serial monitor (115200) to watch"
say "the boot: arduino-cli monitor -p $PORT -b $FQBN --config baudrate=115200"
exit 0
