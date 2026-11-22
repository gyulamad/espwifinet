<?php
// config.example.php — copy this file to config.php and edit the values.
// (config.php is gitignored; index.php refuses to run without it.)

return [
    // Base URL of the relay's HTTP API (no trailing slash). On the same Pi
    // this is localhost; the PHP app and the relay may also run on
    // different machines.
    'relay_url' => 'http://127.0.0.1:8080',

    // The same value as relayKey in nodejs/esp-relay/config.js.
    'relay_key' => 'CHANGE-ME-relay-key',

    // Dashboard login accounts: username => password_hash.
    // Generate a hash once and paste it:
    //   php -r "echo password_hash('YourPassword', PASSWORD_DEFAULT);"
    'users' => [
        'admin' => 'REPLACE-WITH-password_hash-OUTPUT',
    ],

    // Session hardening knobs (defaults are fine).
    'session_name' => 'espdash',
];
