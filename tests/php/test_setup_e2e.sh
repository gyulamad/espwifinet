#!/usr/bin/env bash
# test_setup_e2e.sh — drives the REAL setup.php through php -S (#144):
#   scenario 1: the espdash folder is NOT writable by the web server user
#               (the exact #144 install failure: a root-owned folder after
#               `sudo cp`) -> the page must warn up front, the save must
#               fail with a message that names the fix, and no config.php
#               may appear and no temp file may be left behind;
#   scenario 2: the folder IS writable -> the save succeeds (config.php in
#               place, valid array) and the page locks itself (redirect to
#               index.php).
# Exits 0 when php or curl is missing (run_tests.sh guards this too), so
# machines without the dashboard's runtime do not fail the whole suite.

set -u

command -v php >/dev/null 2>&1 || { echo "SKIP: php not available"; exit 0; }
command -v curl >/dev/null 2>&1 || { echo "SKIP: curl not available"; exit 0; }

repo="$(cd "$(dirname "$0")/../.." && pwd)"
tmp="$(mktemp -d)"
stub_port=18097
app_port=18098
stub_pid=""
app_pid=""
fail=0

cleanup() {
  [ -n "$stub_pid" ] && kill "$stub_pid" 2>/dev/null
  [ -n "$app_pid" ] && kill "$app_pid" 2>/dev/null
  chmod -R u+rwX "$tmp" 2>/dev/null
  rm -rf "$tmp"
}
trap cleanup EXIT

# a tiny router that answers the relay's status API so the probe succeeds
mkdir -p "$tmp/stub"
cat > "$tmp/stub/router.php" <<'PHP'
<?php
header('Content-Type: application/json');
if ($_SERVER['REQUEST_URI'] === '/api/status') { echo '{"devices":[]}'; exit; }
http_response_code(404);
echo '{}';
PHP

cp -r "$repo/php/espdash" "$tmp/espdash"

php -S 127.0.0.1:$stub_port "$tmp/stub/router.php" >/dev/null 2>&1 &
stub_pid=$!
php -S 127.0.0.1:$app_port -t "$tmp/espdash" >/dev/null 2>&1 &
app_pid=$!

for _ in $(seq 1 20); do
  curl -s -o /dev/null "http://127.0.0.1:$app_port/setup.php" && break
  sleep 0.25
done

jar="$tmp/cookies.txt"
relay="http://127.0.0.1:$stub_port"

csrf_from() { printf '%s' "$1" | grep -o 'name="csrf" value="[0-9a-f]*"' | head -1 | grep -o '[0-9a-f]\{16,\}'; }

# ── scenario 1: read-only folder (the #144 failure) ──
chmod 0555 "$tmp/espdash"
page="$(curl -s -c "$jar" "http://127.0.0.1:$app_port/setup.php")"
case "$page" in
  *"cannot write to this folder"*) echo "PASS  read-only dir: page warns before saving";;
  *) echo "FAIL  read-only dir: no writability warning on the page"; fail=1;;
esac
csrf="$(csrf_from "$page")"
if [ -z "$csrf" ]; then echo "FAIL  read-only dir: could not read the csrf token"; fail=1; fi
resp="$(curl -s -b "$jar" -d "csrf=$csrf" -d "relay_url=$relay" -d "relay_key=valid-key-123" \
  -d "username=admin" -d "password=password123" -d "password_confirm=password123" \
  "http://127.0.0.1:$app_port/setup.php")"
case "$resp" in
  *"could not create a temporary file"*) echo "PASS  read-only dir: save fails";;
  *) echo "FAIL  read-only dir: save did not fail"; fail=1;;
esac
case "$resp" in
  *"sudo chown www-data:www-data"*) echo "PASS  read-only dir: failure names the fix";;
  *) echo "FAIL  read-only dir: fix command missing from the message"; fail=1;;
esac
if [ -e "$tmp/espdash/config.php" ]; then echo "FAIL  read-only dir: config.php was written"; fail=1
else echo "PASS  read-only dir: no config.php written"; fi
if ls "$tmp/espdash"/.config.php.tmp-* >/dev/null 2>&1; then echo "FAIL  read-only dir: temp file left behind"; fail=1
else echo "PASS  read-only dir: no temp leftovers"; fi

# ── scenario 2: writable folder ──
chmod 0755 "$tmp/espdash"
rm -f "$jar"
page="$(curl -s -c "$jar" "http://127.0.0.1:$app_port/setup.php")"
csrf="$(csrf_from "$page")"
if [ -z "$csrf" ]; then echo "FAIL  writable dir: could not read the csrf token"; fail=1; fi
resp="$(curl -s -b "$jar" -d "csrf=$csrf" -d "relay_url=$relay" -d "relay_key=valid-key-123" \
  -d "username=admin" -d "password=password123" -d "password_confirm=password123" \
  "http://127.0.0.1:$app_port/setup.php")"
case "$resp" in
  *"Setup complete"*) echo "PASS  writable dir: setup completes";;
  *) echo "FAIL  writable dir: no Setup complete"; fail=1;;
esac
if [ -e "$tmp/espdash/config.php" ]; then echo "PASS  writable dir: config.php exists"
else echo "FAIL  writable dir: config.php missing"; fail=1; fi
if php -r '$c = require $argv[1]; exit((is_array($c) && isset($c["relay_url"], $c["users"]["admin"])) ? 0 : 1);' \
     "$tmp/espdash/config.php" 2>/dev/null; then
  echo "PASS  config.php returns a valid array"
else
  echo "FAIL  config.php invalid"; fail=1
fi
loc="$(curl -s -o /dev/null -w '%{redirect_url}' -b "$jar" "http://127.0.0.1:$app_port/setup.php")"
case "$loc" in
  *index.php) echo "PASS  setup page locks itself after success";;
  *) echo "FAIL  setup page did not redirect after success (got: $loc)"; fail=1;;
esac

if [ "$fail" -eq 0 ]; then
  echo "php setup e2e: ALL PASSED"
else
  echo "php setup e2e: FAILED"
fi
exit "$fail"
