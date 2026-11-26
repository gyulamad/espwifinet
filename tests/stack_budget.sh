#!/usr/bin/env bash
# stack_budget.sh (task #152) - the firmware's stack frames stay small.
#
# Why this exists: the Arduino loop task has only ~4 KB of stack on the
# ESP8266 (8 KB on the ESP32). The setup form's Save once ran
# serve() -> SetupForm::save() -> WifiStore::writeBlob() with a 1 KB read
# chunk, 30 rows of std::string and a 3000-byte flash blob all on that
# stack (~10 KB in total): the node reset in the middle of the request and
# the phone's page hung forever. The desktop tests run on an 8 MB stack, so
# no assert could ever see it.
#
# The check compiles the real sketch + headers against the stubs for BOTH
# boards with -Os (what the Arduino cores use) and -fstack-usage, then
# reads each function's frame size:
#   1. no firmware function may have a frame above FRAME_MAX bytes - a big
#      buffer belongs on the heap (see RegionScratch in wifi_store.h) or in
#      a member of a heap-allocated object;
#   2. the POST /save call chain (serve + save + the larger of the two
#      flash writers) stays within PATH_MAX bytes - the one path that
#      combines the request reader, the form and the flash write.
# x86-64 frames are the same order of magnitude as Xtensa's (the arrays
# dominate), so the limits are deliberately generous and only catch the
# real mistake: a multi-kilobyte local.
#
# Usage: tests/stack_budget.sh        (FRAME_MAX / PATH_MAX overridable)
set -u
cd "$(dirname "$0")/.."

FRAME_MAX="${FRAME_MAX:-1024}"
PATH_MAX="${PATH_MAX:-2048}"

if ! command -v g++ >/dev/null 2>&1; then
  echo "stack budget skipped (no g++)"
  exit 0
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
fail=0

for board in ESP8266 ESP32; do
  inc=(-Itests/cpp/stubs)
  if [ "$board" = "ESP32" ]; then inc=(-Itests/cpp/stubs/esp32 -Itests/cpp/stubs); fi
  if ! g++ -std=gnu++17 -Os -fstack-usage "-D$board" "${inc[@]}" \
        -c tests/cpp/test_net_logic.cpp -o "$tmp/$board.o" 2>"$tmp/$board.err"; then
    echo "FAIL stack budget ($board): the build failed"
    head -20 "$tmp/$board.err"
    fail=1
    continue
  fi

  # .su line: <file>:<line>:<col>:<function>\t<bytes>\t<static|dynamic...>
  report="$(awk -F'\t' -v board="$board" -v frameMax="$FRAME_MAX" -v pathMax="$PATH_MAX" '
    $1 !~ /arduino\/ESP_node\// { next }
    {
      name = $1; sub(/^.*arduino\/ESP_node\//, "", name)
      bytes = $2 + 0
      if (bytes > frameMax) {
        printf "FAIL %s: %d B frame (limit %d): %s\n", board, bytes, frameMax, name
        bad = 1
      }
      if (name ~ /ArduinoSetupNet::serve/)  serve = bytes
      if (name ~ /SetupForm::save/)         save = bytes
      if (name ~ /WifiStore::writeBlob/)    blob = bytes
      if (name ~ /RelayConfigStore::save/)  relay = bytes
    }
    END {
      writer = (blob > relay) ? blob : relay
      chain = serve + save + writer
      if (chain > pathMax) {
        printf "FAIL %s: POST /save chain %d B (serve %d + save %d + flash writer %d), limit %d\n",
               board, chain, serve, save, writer, pathMax
        bad = 1
      } else {
        printf "ok   %s: POST /save chain %d B of %d (serve %d + save %d + flash writer %d)\n",
               board, chain, pathMax, serve, save, writer
      }
      exit bad
    }' "$tmp/$board.su")" || fail=1
  echo "$report"
done

if [ "$fail" -eq 0 ]; then
  echo "stack budget: PASS"
else
  echo "stack budget: FAIL - move the big buffer to the heap (RegionScratch / std::vector) or into a member"
fi
exit "$fail"
