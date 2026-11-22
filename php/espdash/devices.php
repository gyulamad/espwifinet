<?php
// devices.php — the full fleet view: connection status with SSID and signal
// strength (the task's requirement), plus firmware, uptime, heap and GPIO.

require __DIR__ . '/app.php';
Auth::requireLogin();

$relay = relayClient();
$errors = [];
$res = $relay->get('/api/status');
if ($res['ok'] && is_array($res['body'])) {
    $body = $res['body'];
} else {
    $errors[] = $res['error'] ?? 'The relay did not answer.';
    $body = null;
}

Layout::header('Devices', 'devices');
Layout::messages($errors, []);
if ($body === null) {
    Layout::footer();
    exit;
}

$devices = isset($body['devices']) && is_array($body['devices']) ? $body['devices'] : [];

function fmtUptime($ms)
{
    if (!is_numeric($ms) || $ms < 0) {
        return '—';
    }
    $s = (int)($ms / 1000);
    $d = intdiv($s, 86400);
    $h = intdiv($s % 86400, 3600);
    $m = intdiv($s % 3600, 60);
    if ($d > 0) {
        return $d . 'd ' . $h . 'h';
    }
    if ($h > 0) {
        return $h . 'h ' . $m . 'm';
    }
    return $m . 'm';
}

function fmtBytes($n)
{
    if (!is_numeric($n) || $n < 0) {
        return '—';
    }
    return number_format($n / 1024, 1) . ' kB';
}

echo '<section class="card">' . "\n";
echo '<h1>Nodes</h1>' . "\n";
echo '<p class="muted">GPIO pin: ' . e($body['gpioPin'] ?? '?') . ' · desired switch state: <strong>' . e($body['desiredGpio'] ?? '?') . '</strong></p>' . "\n";
echo '<div class="table-scroll"><table>' . "\n";
echo '<thead><tr><th>Node</th><th>Status</th><th>WiFi (SSID)</th><th>Signal</th><th>IP</th><th>Firmware</th><th>Uptime</th><th>Free heap</th><th>GPIO as reported</th><th>Connects</th><th>Last drop reason</th></tr></thead>' . "\n";
echo '<tbody>' . "\n";
foreach ($devices as $dev) {
    $bars = max(0, min(4, (int)($dev['signalBars'] ?? 0)));
    echo '<tr>';
    echo '<td>' . e($dev['name']) . '</td>';
    echo '<td><span class="pill ' . (!empty($dev['online']) ? 'on' : 'off') . '">' . (!empty($dev['online']) ? 'online' : 'offline') . '</span></td>';
    echo '<td>' . e($dev['ssid'] ?? '—') . '</td>';
    echo '<td>' . e(str_repeat('▂', $bars)) . ' ' . e(($dev['rssi'] ?? null) !== null ? $dev['rssi'] . ' dBm' : '') . '</td>';
    echo '<td>' . e($dev['ip'] ?? '—') . '</td>';
    echo '<td>' . e($dev['fwVersion'] ?? '—') . '</td>';
    echo '<td>' . e(fmtUptime($dev['uptimeMs'] ?? null)) . '</td>';
    echo '<td>' . e(fmtBytes($dev['heapFree'] ?? null)) . '</td>';
    echo '<td>' . e(($dev['gpioState'] ?? null) !== null ? 'pin ' . $dev['gpioPin'] . ' = ' . $dev['gpioState'] : '—') . '</td>';
    echo '<td>' . e($dev['connectCount'] ?? '—') . '</td>';
    echo '<td>' . e($dev['lastDisconnectReason'] ?? '—') . '</td>';
    echo '</tr>' . "\n";
}
if (count($devices) === 0) {
    echo '<tr><td colspan="11">No node has connected yet.</td></tr>' . "\n";
}
echo '</tbody></table></div>' . "\n";
echo '</section>' . "\n";
Layout::footer();
