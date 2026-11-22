<?php
// index.php — the dashboard. The task's headline feature lives here: ONE
// On/Off toggle that switches the settings-defined GPIO leg on every node
// (online ones get the command immediately; offline ones catch up on their
// next reconnect — the relay re-sends the desired state on every connect).

require __DIR__ . '/app.php';
Auth::requireLogin();

$relay = relayClient();
$errors = [];
$notices = [];

// Toggle handler (PRG: act, then redirect so a reload never re-toggles).
if (($_SERVER['REQUEST_METHOD'] ?? '') === 'POST') {
    requirePost();
    $state = ($_POST['state'] ?? '') === 'on' ? 'off' : 'on';
    $res = $relay->postJson('/api/gpio', ['state' => $state]);
    if ($res['ok'] && isset($res['body']['ok'])) {
        $delivered = $res['body']['delivered'] ?? 0;
        $notices[] = 'Switched every node ' . strtoupper($state) . ' (delivered to ' . $delivered . ' online node(s); the rest catch up on reconnect).';
        $_SESSION['gpio_notice'] = $notices[0];
        header('Location: index.php');
        exit;
    }
    $errors[] = $res['error'] ?? 'The relay refused the toggle.';
    $_SESSION['gpio_errors'] = $errors;
    header('Location: index.php?flash=1');
    exit;
}

// Flash messages survive the redirect in the session.
if (isset($_SESSION['gpio_notice'])) {
    $notices[] = $_SESSION['gpio_notice'];
    unset($_SESSION['gpio_notice']);
}
if (isset($_SESSION['gpio_errors'])) {
    $errors = array_merge($errors, $_SESSION['gpio_errors']);
    unset($_SESSION['gpio_errors']);
}

$status = $relay->get('/api/status');
if (!$status['ok'] || !is_array($status['body'])) {
    $errors[] = $status['error'] ?? 'The relay did not answer.';
    $statusBody = null;
} else {
    $statusBody = $status['body'];
}

Layout::header('Dashboard', 'dashboard');
Layout::messages($errors, $notices);

if ($statusBody === null) {
    echo '<section class="card"><p>Start the relay (nodejs/esp-relay) and reload.</p></section>' . "\n";
    Layout::footer();
    exit;
}

$online = 0;
$devices = isset($statusBody['devices']) && is_array($statusBody['devices']) ? $statusBody['devices'] : [];
foreach ($devices as $dev) {
    if (!empty($dev['online'])) {
        $online++;
    }
}
$desired = ($statusBody['desiredGpio'] ?? 'off') === 'on' ? 'on' : 'off';
$gpioPin = e($statusBody['gpioPin'] ?? '?');
$total = count($devices);

echo '<section class="card toggle-card">' . "\n";
echo '<h1>Power</h1>' . "\n";
echo '<p class="muted">One switch for the whole network: GPIO ' . $gpioPin . ' on all nodes' . ($online < $total ? ' — ' . $online . ' of ' . $total . ' node(s) online right now' : '') . '.</p>' . "\n";
echo '<form method="post" action="index.php" class="toggle-form">' . "\n";
echo Auth::csrfField() . "\n";
echo '<input type="hidden" name="state" value="' . e($desired) . '">' . "\n";
echo '<button type="submit" id="power-toggle" class="toggle ' . ($desired === 'on' ? 'is-on' : 'is-off') . '">' . "\n";
echo '<span class="toggle-knob"></span><span class="toggle-label">' . ($desired === 'on' ? 'ON' : 'OFF') . '</span>' . "\n";
echo '</button>' . "\n";
echo '</form>' . "\n";
echo '<p class="muted small">Clicking the switch sends ' . ($desired === 'on' ? 'OFF' : 'ON') . ' to every node.</p>' . "\n";
echo '</section>' . "\n";

echo '<section class="card">' . "\n";
echo '<h2>Node status</h2>' . "\n";
echo '<div class="table-scroll"><table>' . "\n";
echo '<thead><tr><th>Node</th><th>State</th><th>WiFi</th><th>Signal</th></tr></thead><tbody>' . "\n";
foreach ($devices as $dev) {
    echo '<tr>';
    echo '<td>' . e($dev['name']) . '</td>';
    echo '<td><span class="pill ' . (!empty($dev['online']) ? 'on' : 'off') . '">' . (!empty($dev['online']) ? 'online' : 'offline') . '</span></td>';
    echo '<td>' . e($dev['ssid'] ?? '—') . '</td>';
    echo '<td>' . e(str_repeat('▂', (int)($dev['signalBars'] ?? 0)) ?: '—') . ' ' . e(($dev['rssi'] ?? null) !== null ? $dev['rssi'] . ' dBm' : '') . '</td>';
    echo '</tr>' . "\n";
}
if ($total === 0) {
    echo '<tr><td colspan="4">No node has connected yet — flash the sketch (see Firmware) and power a node on.</td></tr>' . "\n";
}
echo '</tbody></table></div>' . "\n";
echo '</section>' . "\n";
Layout::footer();
