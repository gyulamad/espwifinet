#!/usr/bin/env bash
# ota-deploy.sh — automates section 5 of INSTALL.md (an OTA firmware release,
# task #159): compile the sketch for the board(s) you name, hand the binaries
# to the relay's firmware API, and — with --wait — watch the fleet until the
# online nodes run the new version. The nodes poll the relay about once a
# minute, download, flash, log every step and reboot by themselves.
#
# Usage:
#   ./ota-deploy.sh [esp8266|esp32|all] [--version V] [--wait SECS]
#
#   esp8266|esp32|all   which image(s) to build and publish (default: all)
#   --version V         the version to publish (default: git describe --always,
#                       with ".t<epoch>" added on a dirty tree so a dev build
#                       always re-flashes — the node updates whenever the
#                       served version DIFFERS from its own, so this also
#                       rolls back)
#   --wait SECS         after publishing, poll /api/status until every ONLINE
#                       node reports the new version, or SECS seconds pass;
#                       exits non-zero if any node is not updated
#
# Needs: arduino-cli (the core for each board you deploy), curl, git.
# Settings: ota.conf (copy ota.example.conf and fill in RELAY_URL + RELAY_KEY;
# the key is sent from a mode-600 header file, never on a command line where
# `ps` would show it).
#
# Exit code: 0 all good, 1 failed (build, upload, or a node not updated),
# 2 wrong usage / missing setup.
set -uo pipefail

cd "$(dirname "$0")" || exit 1

CONF="${OTA_CONF:-./ota.conf}"

say() { echo "ota-deploy: $*"; }
die() { echo "ota-deploy: FAILED: $*" >&2; exit 1; }
usage() { sed -n '2,/^set -uo/p' "${BASH_SOURCE[0]}" | sed '$d' | sed 's/^# \{0,1\}//'; }

# ── arguments ────────────────────────────────────────────────────────────
WHAT="all"
VERSION_ARG=""
WAIT_SECONDS=0
while [ $# -gt 0 ]; do
  case "$1" in
    esp8266|esp32|all) WHAT="$1"; shift ;;
    --version) [ $# -ge 2 ] || { echo "--version needs a value" >&2; exit 2; }; VERSION_ARG="$2"; shift 2 ;;
    --version=*) VERSION_ARG="${1#--version=}"; shift ;;
    --wait) [ $# -ge 2 ] || { echo "--wait needs a number of seconds" >&2; exit 2; }; WAIT_SECONDS="$2"; shift 2 ;;
    --wait=*) WAIT_SECONDS="${1#--wait=}"; shift ;;
    -h|--help) usage; exit 0 ;;
    *) echo "ota-deploy: unknown argument: $1 (try --help)" >&2; exit 2 ;;
  esac
done
[[ "$WAIT_SECONDS" =~ ^[0-9]+$ ]] || { echo "ota-deploy: --wait must be a whole number of seconds" >&2; exit 2; }

# ── settings ─────────────────────────────────────────────────────────────
[ -f "$CONF" ] || { echo "ota-deploy: no $CONF - copy ota.example.conf to ota.conf and fill in RELAY_URL + RELAY_KEY" >&2; exit 2; }
# shellcheck disable=SC1090
. "$CONF"
[ -n "${RELAY_URL:-}" ] || { echo "ota-deploy: RELAY_URL is not set in $CONF" >&2; exit 2; }
[ -n "${RELAY_KEY:-}" ] || { echo "ota-deploy: RELAY_KEY is not set in $CONF" >&2; exit 2; }
RELAY_URL="${RELAY_URL%/}"
FQBN_ESP8266="${FQBN_ESP8266:-esp8266:esp8266:nodemcuv2}"
FQBN_ESP32="${FQBN_ESP32:-esp32:esp32:esp32}"

for tool in arduino-cli curl git node; do
  command -v "$tool" >/dev/null 2>&1 || { echo "ota-deploy: $tool not found on PATH" >&2; exit 2; }
done

# ── what to build ────────────────────────────────────────────────────────
case "$WHAT" in
  all)     CHIPS="esp8266 esp32" ;;
  *)       CHIPS="$WHAT" ;;
esac

# ── version ──────────────────────────────────────────────────────────────
if [ -n "$VERSION_ARG" ]; then
  VERSION="$VERSION_ARG"
else
  VERSION="$(git describe --always --dirty 2>/dev/null)" || die "git describe failed - is this a git checkout?"
  case "$VERSION" in
    *-dirty)
      VERSION="$VERSION.t$(date +%s)"
      say "WARNING: uncommitted changes - deploying a dev build ($VERSION). It will re-flash every time." ;;
  esac
fi
# The relay accepts 1-32 characters of A-Z a-z 0-9 . _ - (firmware.js).
if ! [[ "$VERSION" =~ ^[A-Za-z0-9._-]{1,32}$ ]]; then
  die "the version '$VERSION' has characters or a length the relay does not accept (1-32 of A-Z a-z 0-9 . _ -)"
fi
say "Version: $VERSION"

# ── build (everything first: no half deployments) ────────────────────────
TMP="$(mktemp -d)"
cleanup() { rm -rf "$TMP"; }
trap cleanup EXIT
trap 'exit 130' INT TERM

# The key goes into a header file (mode 600), not onto curl's command line
# where `ps` shows it.
umask 077
printf 'X-API-Key: %s\n' "$RELAY_KEY" > "$TMP/api-header"

for chip in $CHIPS; do
  case "$chip" in
    esp8266) FQBN="$FQBN_ESP8266" ;;
    esp32)   FQBN="$FQBN_ESP32" ;;
  esac
  say "== Compiling $chip ($FQBN)"
  arduino-cli compile --fqbn "$FQBN" --output-dir "$TMP/$chip" arduino/ESP_node || die "compiling for $chip failed"
  bin="$TMP/$chip/ESP_node.ino.bin"
  [ -f "$bin" ] || die "the build did not produce ESP_node.ino.bin for $chip (looked in $TMP/$chip)"
  # The relay refuses a file without the ESP image magic at upload time —
  # catch it here, with the local path in the message, instead.
  size="$(wc -c < "$bin" | tr -d ' ')"
  [ "$(head -c 1 "$bin" | od -An -tx1 | tr -d ' \n')" = "e9" ] || die "$TMP/$chip/ESP_node.ino.bin does not start with the ESP image magic 0xE9 - not a flashable image"
  say "   built $bin ($size bytes)"
done

# ── publish ──────────────────────────────────────────────────────────────
for chip in $CHIPS; do
  say "== Publishing $chip $VERSION to $RELAY_URL"
  code="$(curl -sS -o "$TMP/reply-$chip" -w '%{http_code}' -X POST \
          -H "@$TMP/api-header" -H 'Content-Type: application/octet-stream' \
          --data-binary "@$TMP/$chip/ESP_node.ino.bin" \
          "$RELAY_URL/api/firmware?chip=$chip&version=$VERSION")" \
          || die "could not reach the relay at $RELAY_URL"
  if [ "$code" != "200" ]; then
    echo "   the relay answered HTTP $code: $(cat "$TMP/reply-$chip")" >&2
    die "publishing the $chip firmware failed"
  fi
  say "   published: $(cat "$TMP/reply-$chip")"
done

if [ "$WAIT_SECONDS" -eq 0 ]; then
  echo
  say "Published. The nodes check about once a minute; watch the dashboard's"
  say "Devices page (Firmware column) or the relay's Logs page."
  exit 0
fi

# ── wait and report ──────────────────────────────────────────────────────
# GET /api/status: devices[] with deviceId, online, fwVersion. The verdict:
# at least one node online, and every ONLINE node runs THIS version (offline
# nodes cannot download — they are reported, not failed on).
say "Waiting up to ${WAIT_SECONDS}s for the online nodes to update (they check about once a minute)"
deadline=$(( $(date +%s) + WAIT_SECONDS ))
all_updated=0
while :; do
  if curl -sS -f -H "@$TMP/api-header" "$RELAY_URL/api/status" -o "$TMP/status" 2>/dev/null; then
    verdict="$(node -e '
      const s = JSON.parse(require("fs").readFileSync(process.argv[1], "utf8"));
      const devs = (s.devices || []);
      const online = devs.filter((d) => d.online);
      const pending = online.filter((d) => d.fwVersion !== process.argv[2]);
      process.exit(online.length > 0 && pending.length === 0 ? 0 : 1);
    ' "$TMP/status" "$VERSION" 2>/dev/null)" && { all_updated=1; break; }
  fi
  remaining=$(( deadline - $(date +%s) ))
  [ "$remaining" -gt 0 ] || break
  poll="${OTA_POLL_SECONDS:-10}"
  sleep $(( remaining < poll ? remaining : poll ))
done

echo
printf 'device\t\t\tonline\trunning version\n' > "$TMP/table"
node -e '
  const s = JSON.parse(require("fs").readFileSync(process.argv[1], "utf8"));
  for (const d of s.devices || []) {
    console.log((d.deviceId || "?") + "\t" + (d.name || "") + "\t" + (d.online ? "online" : "offline") + "\t" + (d.fwVersion || "(none reported)"));
  }
' "$TMP/status" >> "$TMP/table" 2>/dev/null || true
if [ "$(wc -l < "$TMP/table")" -le 1 ]; then
  say "No node has talked to the relay since it started (or /api/status was unreachable)."
else
  column -t -s "$(printf '\t')" "$TMP/table" 2>/dev/null || cat "$TMP/table"
fi
echo
if [ "$all_updated" = 1 ]; then
  say "All online nodes run $VERSION."
  exit 0
fi
say "NOT every online node runs $VERSION yet. Offline nodes cannot download (WiFi /"
say "power?) — they update the moment they reconnect. They keep checking every"
say "minute; run again with --wait to check later."
exit 1
