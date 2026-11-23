<?php
// settings.php — everything the operator configures, in one place (#36): the
// switch output (GPIO pin, active level), node reporting (status cadence,
// lowest queued log level), the offline window (was hard-coded in the
// relay's config.js) and the per-node friendly names. Saving posts the whole
// settings object to the relay, which validates it again, stores it and
// pushes it to every online node immediately. The relay's STARTUP values
// (ports, keys — config.js on the Pi) are shown read-only via /api/config:
// editing them needs a restart, so they live on the Pi.

require __DIR__ . '/app.php';
Auth::requireLogin();

$relay = relayClient();
$errors = [];
$notices = [];

// Load current settings for the form (kept even on a failed save, so the
// operator sees exactly what the relay holds).
$get = $relay->get('/api/settings');
$settings = ($get['ok'] && is_array($get['body']['settings'] ?? null)) ? $get['body']['settings'] : null;
if ($settings === null) {
    $errors[] = $get['error'] ?? 'Could not load the settings — is the relay running?';
}

// System summary (never contains secrets — only "is it set" booleans).
$cfgGet = $relay->get('/api/config');
$system = ($cfgGet['ok'] && is_array($cfgGet['body'] ?? null)) ? $cfgGet['body'] : null;
if ($system === null) {
    // the relay may predate #36; the page still works with the editable fields
    $system = null;
}

if (($_SERVER['REQUEST_METHOD'] ?? '') === 'POST' && $settings !== null) {
    requirePost();

    $save = [
        'gpioPin' => $_POST['gpioPin'] ?? '',
        'gpioActiveHigh' => isset($_POST['gpioActiveHigh']),
        'statusIntervalMs' => (int)($_POST['statusIntervalMs'] ?? 10000),
        'offlineAfterMs' => (int)($_POST['offlineAfterMs'] ?? 45000),
        'minLogLevel' => (string)($_POST['minLogLevel'] ?? 'info'),
        'names' => [],
    ];

    // names arrive as names[<deviceId>] fields; only strings survive.
    $postedNames = $_POST['names'] ?? [];
    if (is_array($postedNames)) {
        foreach ($postedNames as $id => $name) {
            if (is_string($id) && is_string($name) && $name !== '') {
                $save['names'][$id] = $name;
            }
        }
    }

    $res = $relay->postJson('/api/settings', $save);
    if ($res['ok'] && isset($res['body']['ok'])) {
        $settings = $res['body']['settings'];
        foreach (($res['body']['warnings'] ?? []) as $w) {
            $notices[] = 'Saved, with warning: ' . $w;
        }
        if (empty($notices)) {
            $notices[] = 'Settings saved and pushed to all online nodes.';
        }
    } else {
        $bodyErrors = is_array($res['body']['errors'] ?? null) ? $res['body']['errors'] : [];
        $errors = array_merge($errors, $bodyErrors);
        if (empty($errors)) {
            $errors[] = $res['error'] ?? 'The relay refused the settings.';
        }
    }
}

Layout::header('Settings', 'settings');
Layout::messages($errors, $notices);

if ($settings === null) {
    Layout::footer();
    exit;
}

$knownIds = [];
$status = $relay->get('/api/status');
if ($status['ok'] && is_array($status['body']['devices'] ?? null)) {
    foreach ($status['body']['devices'] as $dev) {
        if (isset($dev['deviceId'])) {
            $knownIds[$dev['deviceId']] = $dev;
        }
    }
}
// Names for devices the relay knows plus devices already named in settings.
$allIds = array_unique(array_merge(array_keys($knownIds), array_keys($settings['names'] ?? [])));
sort($allIds);

echo '<section class="card narrow">' . "\n";
echo '<h1>Settings</h1>' . "\n";
echo '<form method="post" action="settings.php">' . "\n";
echo Auth::csrfField() . "\n";
echo '<fieldset>' . "\n";
echo '<legend>Switch output</legend>' . "\n";
echo '<label>GPIO pin (0–16) — the leg every node switches <input name="gpioPin" value="' . e($settings['gpioPin'] ?? 5) . '" inputmode="numeric" pattern="[0-9]{1,2}" required></label>' . "\n";
echo '<label class="check"><input type="checkbox" name="gpioActiveHigh"' . (!empty($settings['gpioActiveHigh']) ? ' checked' : '') . '> active-HIGH (ON = 3.3 V). Untick for active-LOW relay boards.</label>' . "\n";
echo '</fieldset>' . "\n";
echo '<fieldset>' . "\n";
echo '<legend>Node reporting and presence</legend>' . "\n";
echo '<label>Status interval, ms (2000–600000) — how often nodes report WiFi/heap <input name="statusIntervalMs" value="' . e($settings['statusIntervalMs'] ?? 10000) . '" inputmode="numeric" pattern="[0-9]{4,6}" required></label>' . "\n";
echo '<label>Offline window, ms (30000–3600000, at least 3x the status interval) — silence before a node counts as offline <input name="offlineAfterMs" value="' . e($settings['offlineAfterMs'] ?? 45000) . '" inputmode="numeric" pattern="[0-9]{5,7}" required></label>' . "\n";
echo '<label>Lowest log level nodes queue for the relay <select name="minLogLevel">';
foreach (['info', 'warn', 'error'] as $lvl) {
    echo '<option value="' . $lvl . '"' . (($settings['minLogLevel'] ?? 'info') === $lvl ? ' selected' : '') . '>' . $lvl . '</option>';
}
echo '</select></label>' . "\n";
echo '<p class="muted">info queues everything, warn drops info lines, error queues errors only. The node\'s serial output always shows every line. Changes are pushed to online nodes on save.</p>' . "\n";
echo '</fieldset>' . "\n";
echo '<fieldset>' . "\n";
echo '<legend>Node names</legend>' . "\n";
foreach ($allIds as $id) {
    echo '<label>' . e($id) . ' <input name="names[' . e($id) . ']" value="' . e($settings['names'][$id] ?? '') . '" maxlength="40"></label>' . "\n";
}
if (count($allIds) === 0) {
    echo '<p class="muted">No nodes known yet — names can be added once a node has connected.</p>' . "\n";
}
echo '</fieldset>' . "\n";
echo '<button type="submit">Save settings</button>' . "\n";
echo '</form>' . "\n";
echo '</section>' . "\n";
if ($system !== null) {
    echo '<section class="card narrow">' . "\n";
    echo '<h2>Relay system (read-only)</h2>' . "\n";
    echo '<p class="muted">Startup values — edit them in config.js on the Pi and restart the relay there. Nothing secret is shown, only whether each key is set.</p>' . "\n";
    echo '<ul>' . "\n";
    echo '<li>HTTP API port: <code>' . e($system['httpPort'] ?? '?') . '</code></li>' . "\n";
    echo '<li>Node TCP port (RELAY_PORT in the sketch): <code>' . e($system['nodePort'] ?? '?') . '</code></li>' . "\n";
    echo '<li>relayKey set: <code>' . (!empty($system['relayKeySet']) ? 'yes' : 'NO — still the template value') . '</code></li>' . "\n";
    echo '<li>nodeKey set: <code>' . (!empty($system['nodeKeySet']) ? 'yes' : 'NO — still the template value') . '</code></li>' . "\n";
    $fw = $system['firmware'] ?? null;
    echo '<li>Staged OTA firmware: <code>' . e(is_array($fw) && isset($fw['version']) ? $fw['version'] : 'none uploaded') . '</code></li>' . "\n";
    echo '</ul>' . "\n";
    if (isset($system['needsRestart'])) {
        echo '<p class="muted">' . e($system['needsRestart']) . '</p>' . "\n";
    }
    echo '</section>' . "\n";
}
Layout::footer();
