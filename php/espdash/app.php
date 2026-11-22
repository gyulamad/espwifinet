<?php
// app.php — the single bootstrap every page requires first: config load,
// session start, login check, shared library classes. Kept in one file so a
// page can never forget one of the steps (DRY).

declare(strict_types=1);

if (!file_exists(__DIR__ . '/config.php')) {
    http_response_code(500);
    exit('config.php is missing — copy config.example.php to config.php and edit it (see INSTALL.md)');
}
$CONFIG = require __DIR__ . '/config.php';

// Fail loudly on an unedited example config: the default keys would let any
// node/frontend talk to the relay.
if (!isset($CONFIG['relay_url'], $CONFIG['relay_key'], $CONFIG['users'], $CONFIG['session_name'])) {
    http_response_code(500);
    exit('config.php is incomplete — see config.example.php for the required keys');
}
if ($CONFIG['relay_key'] === 'CHANGE-ME-relay-key' || strpos($CONFIG['relay_key'], 'CHANGE-ME') === 0) {
    http_response_code(500);
    exit('config.php still contains the example relay key — set your own (see INSTALL.md)');
}

require __DIR__ . '/lib/Relay.php';
require __DIR__ . '/lib/Auth.php';
require __DIR__ . '/lib/Layout.php';

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
