<?php
// logs.php — per-device remote logs (and the relay's own log). The device
// selector only ever produces values from /api/logs/devices, so the id the
// relay receives is always one it vetted itself.

require __DIR__ . '/app.php';
Auth::requireLogin();

$relay = relayClient();
$errors = [];
$lines = 200;

$idsRes = $relay->get('/api/logs/devices');
$deviceIds = ($idsRes['ok'] && is_array($idsRes['body']['devices'] ?? null)) ? $idsRes['body']['devices'] : [];

// The chosen device must be one of the ids the relay listed (or 'relay').
$device = $_GET['device'] ?? 'relay';
if ($device !== 'relay' && !in_array($device, $deviceIds, true)) {
    $device = 'relay';
}

$entries = [];
$res = $relay->get('/api/logs?device=' . rawurlencode($device) . '&lines=' . (int)$lines);
if ($res['ok'] && is_array($res['body']['entries'] ?? null)) {
    $entries = $res['body']['entries'];
} else {
    $errors[] = $res['error'] ?? 'Could not read the log.';
}

Layout::header('Logs', 'logs');
Layout::messages($errors, []);

echo '<section class="card">' . "\n";
echo '<h1>Logs</h1>' . "\n";
echo '<form method="get" action="logs.php" class="inline-form">' . "\n";
echo '<label>Source <select name="device">' . "\n";
echo '<option value="relay"' . ($device === 'relay' ? ' selected' : '') . '>relay (this Pi)</option>' . "\n";
foreach ($deviceIds as $id) {
    echo '<option value="' . e($id) . '"' . ($id === $device ? ' selected' : '') . '>' . e($id) . '</option>' . "\n";
}
echo '</select></label>' . "\n";
echo '<button type="submit">Show</button>' . "\n";
echo '</form>' . "\n";

echo '<div class="table-scroll"><table class="log-table"><tbody>' . "\n";
foreach ($entries as $entry) {
    $level = in_array($entry['level'] ?? 'info', ['info', 'warn', 'error'], true) ? $entry['level'] : 'info';
    echo '<tr class="log-' . e($level) . '">';
    echo '<td class="log-ts">' . e($entry['ts'] ?? '') . '</td>';
    echo '<td>' . e($entry['device'] ?? '') . '</td>';
    echo '<td><span class="pill ' . e($level === 'error' ? 'off' : ($level === 'warn' ? 'warn' : 'on')) . '">' . e($level) . '</span></td>';
    echo '<td>' . e($entry['msg'] ?? '') . '</td>';
    echo '</tr>' . "\n";
}
if (count($entries) === 0) {
    echo '<tr><td colspan="4">No log entries (newest 200 are shown here).</td></tr>' . "\n";
}
echo '</tbody></table></div>' . "\n";
echo '</section>' . "\n";
Layout::footer();
