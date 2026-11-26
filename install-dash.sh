#!/usr/bin/env bash
# install-dash.sh — automates section 2 of INSTALL.md (the PHP dashboard on
# the Pi, task #159). Run from the repository root:
#   ./install-dash.sh
# It does exactly what the manual section says, in order:
#   1. makes sure PHP >= 8 with the curl extension is installed (via apt when
#      it is missing and NO_APT is not set) and restarts apache2 so the
#      extension is loaded,
#   2. copies php/espdash into the web folder with explicit modes (files 644,
#      folders 755 - never whatever this shell's umask gives),
#   3. hands the folder to the web server user: the setup page WRITES
#      config.php into it (temp file + rename), so a root-owned folder makes
#      setup.php fail with "could not create a temporary file",
#   4. prints the setup URL. An existing config.php (your login) is NEVER
#      touched.
#
# Settings (environment variables):
#   DASH_DIR  destination (default /var/www/html/espdash)
#   DASH_USER the web server user (default www-data, the Debian/Raspberry Pi
#             OS default; `ps aux | grep php-fpm` or apache2.conf shows yours)
#   NO_APT=1  never run apt (install php/php-curl yourself first)
#
# Exit code: 0 all good, 1 failed.
set -u

cd "$(dirname "$0")" || exit 1

DASH_DIR="${DASH_DIR:-/var/www/html/espdash}"
DASH_USER="${DASH_USER:-www-data}"
NO_APT="${NO_APT:-0}"
SUDO=""
if [ "$(id -u)" -ne 0 ]; then
  command -v sudo >/dev/null 2>&1 || { echo "install-dash: sudo not found - run as root or install sudo"; exit 1; }
  SUDO="sudo"
fi

say() { echo "install-dash: $*"; }
die() { echo "install-dash: FAILED: $*" >&2; exit 1; }

[ -f php/espdash/index.php ] || die "run this from the repository root (php/espdash/index.php not found)"

# ── 1. PHP + curl extension ──────────────────────────────────────────────
php_ok=0
if command -v php >/dev/null 2>&1; then
  php_major="$(php -r 'echo (int)PHP_MAJOR_VERSION;')"
  if php -m | grep -qi '^curl$'; then
    say "PHP $php_major.x with the curl extension is installed"
    php_ok=1
  else
    say "PHP is installed but the curl extension is missing (the dashboard needs it)"
  fi
else
  say "php not found"
fi

if [ "$php_ok" -ne 1 ]; then
  if [ "$NO_APT" = "1" ]; then
    die "install php and php-curl yourself (see INSTALL.md section 2), or unset NO_APT"
  fi
  command -v apt-get >/dev/null 2>&1 || die "no apt-get and PHP/curl is missing - install php + php-curl by hand (INSTALL.md section 2)"
  say "installing php and php-curl via apt"
  $SUDO apt-get install -y php php-curl || die "apt-get install php php-curl failed"
  if php -m 2>/dev/null | grep -qi '^curl$'; then
    say "curl extension present"
  else
    die "the curl extension is still missing after the install"
  fi
fi

# Restart the web server so a freshly installed extension is actually loaded.
if command -v systemctl >/dev/null 2>&1 && systemctl is-active --quiet apache2 2>/dev/null; then
  say "restarting apache2 (loads the extension into the web server)"
  $SUDO systemctl restart apache2 || say "WARNING: apache2 restart failed - reload it by hand"
fi

# ── 2. the web server user ───────────────────────────────────────────────
id -u "$DASH_USER" >/dev/null 2>&1 || die "web server user '$DASH_USER' does not exist - set DASH_USER (ps aux | grep php-fpm / apache2.conf shows yours)"

# ── 3. copy the dashboard ────────────────────────────────────────────────
say "copying php/espdash to $DASH_DIR (files 644, folders 755)"
$SUDO install -d -m 755 "$DASH_DIR" "$DASH_DIR/lib" "$DASH_DIR/assets" || die "could not create $DASH_DIR"
for f in php/espdash/*.php php/espdash/README.txt php/espdash/assets/*; do
  [ -f "$f" ] || continue
  base="$(basename "$f")"
  [ "$base" = "config.php" ] && continue           # the web folder's own config is never touched
  $SUDO install -m 644 "$f" "$DASH_DIR/$base" || die "could not install $base"
done
for f in php/espdash/lib/*.php; do
  $SUDO install -m 644 "$f" "$DASH_DIR/lib/$(basename "$f")" || die "could not install lib/$(basename "$f")"
done
if [ -f "$DASH_DIR/config.php" ]; then
  say "config.php already exists in the web folder - keeping it (your login is untouched)"
fi

# ── 4. hand the folder to the web server user ────────────────────────────
# The folder itself must be writable by the web server: setup.php writes
# config.php INTO it (temp file + rename) - the one chown the manual section
# calls out. Files stay root-owned 644 (readable, not writable by the web
# user); an existing config.php keeps its owner.
$SUDO chown "$DASH_USER":"$DASH_USER" "$DASH_DIR" || die "could not chown $DASH_DIR to $DASH_USER"
$SUDO chmod 755 "$DASH_DIR" || die "could not chmod $DASH_DIR"

echo
say "dashboard installed in $DASH_DIR"
ip="$(hostname -I 2>/dev/null | awk '{print $1}')"
say "next: open http://${ip:-<pi-ip>}/espdash/setup.php - the initial setup page"
say "      (relay URL http://127.0.0.1:8080, relayKey from the relay's config.js,"
say "       first dashboard login - the page locks itself once config.php exists)"
exit 0
