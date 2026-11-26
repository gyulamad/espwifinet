#!/usr/bin/env bash
# install-relay.sh — automates section 1 of INSTALL.md (the nodejs relay on
# the Pi, task #159). Run from the repository root:
#   ./install-relay.sh
# It does exactly what the manual section says, in order:
#   1. copies the relay into /opt/espwifinet/esp-relay (only the CODE: an
#      update never touches config.js, data/, logs/ or firmware/),
#   2. runs npm install there,
#   3. creates config.js from example.config.js with two GENERATED keys
#      (an existing config.js is never overwritten — run
#      `node -e ...randomBytes...` and edit by hand to rotate them),
#   4. writes the systemd service and starts it,
#   5. health-checks the relay and opens the firewall ports it needs
#      (7777/tcp for the nodes, 8083/udp for discovery) when ufw is active.
#
# Settings (environment variables):
#   RELAY_DIR     destination (default /opt/espwifinet/esp-relay)
#   SERVICE_USER  the user the service runs as (default: the user running this
#                 script — the relay WRITES its settings and logs there, so it
#                 must not be root)
#   NO_SYSTEMD=1  skip the service install (containers, non-systemd systems)
#
# Exit code: 0 all good, 1 failed.
set -u

cd "$(dirname "$0")" || exit 1

RELAY_DIR="${RELAY_DIR:-/opt/espwifinet/esp-relay}"
NO_SYSTEMD="${NO_SYSTEMD:-0}"
SUDO=""
if [ "$(id -u)" -ne 0 ]; then
  command -v sudo >/dev/null 2>&1 || { echo "install-relay: sudo not found - run as root or install sudo"; exit 1; }
  SUDO="sudo"
fi

say()   { echo "install-relay: $*"; }
die()   { echo "install-relay: FAILED: $*" >&2; exit 1; }

# ── 0. prerequisites ─────────────────────────────────────────────────────
command -v node >/dev/null 2>&1 || die "node not found - install Node.js >= 18 first (see INSTALL.md section 1)"
NODE_MAJOR="$(node -p 'process.versions.node.split(".")[0]')"
[ "$NODE_MAJOR" -ge 18 ] || die "Node.js >= 18 required, this is $(node --version)"
command -v npm >/dev/null 2>&1 || die "npm not found (it ships with Node.js)"
[ -f nodejs/esp-relay/server.js ] || die "run this from the repository root (nodejs/esp-relay/server.js not found)"

SERVICE_USER="${SERVICE_USER:-$(id -un)}"
[ "$SERVICE_USER" = "root" ] && die "SERVICE_USER must be a normal user (the relay writes its settings and logs there) - run this script as your own login"

# ── 1. copy the CODE into the destination ────────────────────────────────
# Explicit modes (files 644, folders 755), not the umask of this shell: under
# a strict umask plain cp would make files 600 and the service user could not
# read them (the same trap deploy scripts fall into, see espcamsys2).
say "copying the relay code to $RELAY_DIR"
$SUDO install -d -m 755 "$RELAY_DIR" "$RELAY_DIR/lib"
for f in server.js package.json package-lock.json example.config.js; do
  [ -f "nodejs/esp-relay/$f" ] || die "nodejs/esp-relay/$f is missing"
  $SUDO install -m 644 "nodejs/esp-relay/$f" "$RELAY_DIR/$f" || die "could not copy $f"
done
for f in nodejs/esp-relay/lib/*.js; do
  $SUDO install -m 644 "$f" "$RELAY_DIR/lib/$(basename "$f")" || die "could not copy lib/$(basename "$f")"
done
$SUDO chown -R "$SERVICE_USER:" "$RELAY_DIR" || die "could not hand $RELAY_DIR to $SERVICE_USER"

# ── 2. dependencies ──────────────────────────────────────────────────────
say "npm install (in $RELAY_DIR)"
(cd "$RELAY_DIR" && npm install --omit=dev --no-audit --no-fund) || die "npm install failed"

# ── 3. config with generated keys ────────────────────────────────────────
if [ -f "$RELAY_DIR/config.js" ]; then
  say "config.js already exists - keeping it (keys and ports untouched)"
else
  say "creating config.js with two generated keys"
  (cd "$RELAY_DIR" && cp example.config.js config.js) || die "could not copy example.config.js"
  (cd "$RELAY_DIR" && node -e '
    const fs = require("fs");
    const key = () => require("crypto").randomBytes(24).toString("hex");
    let s = fs.readFileSync("config.js", "utf8");
    s = s.replace(/relayKey: .CHANGE-ME-relay-key./, "relayKey: \x27" + key() + "\x27");
    s = s.replace(/nodeKey: .CHANGE-ME-node-key./, "nodeKey: \x27" + key() + "\x27");
    if (/CHANGE-ME/.test(s)) { console.error("key replacement failed"); process.exit(1); }
    fs.writeFileSync("config.js", s, { mode: 0o600 });
  ') || die "could not write config.js"
fi

# ── 4. the systemd service ───────────────────────────────────────────────
if [ "$NO_SYSTEMD" = "1" ]; then
  say "NO_SYSTEMD=1 - skipping the service install; start it by hand:"
  say "  cd $RELAY_DIR && node server.js"
else
  command -v systemctl >/dev/null 2>&1 || die "systemctl not found and NO_SYSTEMD is not set - export NO_SYSTEMD=1 to skip the service"
  say "writing /etc/systemd/system/esp-relay.service (User: $SERVICE_USER)"
  printf '%s\n' \
    "[Unit]" \
    "Description=ESP Wifi Network relay" \
    "After=network-online.target" \
    "" \
    "[Service]" \
    "WorkingDirectory=$RELAY_DIR" \
    "ExecStart=$(command -v node) server.js" \
    "Restart=always" \
    "User=$SERVICE_USER" \
    "" \
    "[Install]" \
    "WantedBy=multi-user.target" \
    | $SUDO tee /etc/systemd/system/esp-relay.service >/dev/null || die "could not write the service unit"
  $SUDO systemctl daemon-reload || die "systemctl daemon-reload failed"
  $SUDO systemctl enable --now esp-relay || die "could not enable/start esp-relay"
  sleep 2
  # ── 5. health check ─────────────────────────────────────────────────
  ok=0
  for i in 1 2 3 4 5; do
    if curl -sf "http://127.0.0.1:8080/healthz" >/dev/null 2>&1; then ok=1; break; fi
    sleep 1
  done
  if [ "$ok" = 1 ]; then
    say "the relay is up (http://127.0.0.1:8080/healthz answers)"
  else
    say "WARNING: the health check did not answer yet - look at: journalctl -u esp-relay -n 50"
  fi
fi

# ── 6. firewall ──────────────────────────────────────────────────────────
if command -v ufw >/dev/null 2>&1 && $SUDO ufw status 2>/dev/null | grep -q "Status: active"; then
  say "ufw is active - opening 7777/tcp (nodes) and 8083/udp (discovery)"
  $SUDO ufw allow 7777/tcp >/dev/null || die "ufw allow 7777/tcp failed"
  $SUDO ufw allow 8083/udp >/dev/null || die "ufw allow 8083/udp failed"
else
  say "no active ufw found - make sure 7777/tcp (nodes) and 8083/udp (discovery) can reach the Pi"
fi

# ── done: what the operator needs once ───────────────────────────────────
echo
say "relay installed in $RELAY_DIR"
if [ -f "$RELAY_DIR/config.js" ]; then
  echo "  keys (from config.js, generated on first install - needed for the"
  echo "  dashboard's setup page and the node's setup page):"
  node -e '
    const c = require(process.argv[1] + "/config.js");
    console.log("  relayKey (PHP dashboard -> setup page): " + c.relayKey);
    console.log("  nodeKey  (every node -> its setup page): " + c.nodeKey);
    if (/CHANGE-ME/.test(c.relayKey + c.nodeKey)) console.log("  WARNING: keys still say CHANGE-ME - edit config.js!");
  ' "$RELAY_DIR"
fi
echo "  next: install the dashboard with ./install-dash.sh (INSTALL.md section 2)"
exit 0
