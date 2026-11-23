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
