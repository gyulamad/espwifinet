<?php
// app.php — the single bootstrap every page requires first: config load,
// session start, login check, shared library classes. Kept in one file so a
// page can never forget one of the steps (DRY).
//
// A missing or still-template config is NOT a hard error since #36: every
// page routes to setup.php, the initial setup page that writes config.php
// (see the file header there). Only setup.php itself runs without config.

declare(strict_types=1);

function dash_route_to_setup(): void
{
    header('Location: setup.php');
    exit;
}

if (!file_exists(__DIR__ . '/config.php')) {
    dash_route_to_setup();
}
$CONFIG = require __DIR__ . '/config.php';

// Fail over to setup on an unedited example config: the default keys would
// let any node/frontend talk to the relay.
if (!isset($CONFIG['relay_url'], $CONFIG['relay_key'], $CONFIG['users'], $CONFIG['session_name'])) {
    dash_route_to_setup();
}
if (!is_array($CONFIG['users']) || count($CONFIG['users']) === 0) {
    dash_route_to_setup();
}
if ($CONFIG['relay_key'] === 'CHANGE-ME-relay-key' || strpos($CONFIG['relay_key'], 'CHANGE-ME') === 0) {
    dash_route_to_setup();
}

require __DIR__ . '/lib/Relay.php';
require __DIR__ . '/lib/Auth.php';
require __DIR__ . '/lib/Layout.php';

// #149: every page talks to the relay through PHP's curl extension. A
// missing extension used to fatal deep inside Relay.php ("Call to
// undefined function curl_init()") with a stack trace an installer cannot
// act on - say so here, once, with the exact fix.
if (!extension_loaded('curl')) {
    http_response_code(500);
    Layout::header('The PHP curl extension is missing', 'setup');
    echo '<section class="card narrow">' . "\n";
    echo '<h1>The PHP curl extension is missing</h1>' . "\n";
    echo '<p class="msg err">This dashboard talks to the relay through PHP\'s curl extension, and this web server\'s PHP does not have it loaded — every page would fail with "Call to undefined function curl_init()".</p>' . "\n";
    echo '<p>Install it on the Pi, then reload this page:</p>' . "\n";
    echo '<pre><code>sudo apt install php-curl' . "\n" . 'sudo systemctl restart apache2</code></pre>' . "\n";
    echo '<p class="muted">On Debian/Raspberry Pi OS one package covers the command-line PHP and the web server module; the restart is what loads it into Apache.</p>' . "\n";
    echo '</section>' . "\n";
    Layout::footer();
    exit;
}

Auth::start($CONFIG['session_name']);

function relayClient(): Relay
{
    global $CONFIG;
    return new Relay($CONFIG['relay_url'], $CONFIG['relay_key']);
}

function configValue(string $key)
{
    global $CONFIG;
    return $CONFIG[$key];
}

// POST guard used by every page that changes state: CSRF first, then the
// login check (defence in depth — requireLogin() already ran on GET).
function requirePost()
{
    if (($_SERVER['REQUEST_METHOD'] ?? '') !== 'POST') {
        http_response_code(405);
        exit('POST required');
    }
    Auth::verifyCsrf();
}
