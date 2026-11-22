<?php
// ota.php — firmware page: what version the relay currently serves, and the
// upload form. Nodes poll /firmware/version on the relay and self-update
// when the served version differs from theirs (see arduino/ESP8266_Node).

require __DIR__ . '/app.php';
Auth::requireLogin();

$relay = relayClient();
$errors = [];
$notices = [];

if (($_SERVER['REQUEST_METHOD'] ?? '') === 'POST') {
    requirePost();

    $version = trim($_POST['version'] ?? '');
    if (!preg_match('/^[A-Za-z0-9._-]{1,32}$/', $version)) {
        $errors[] = 'Version must be 1-32 characters of letters, digits, dot, dash or underscore.';
    }

    // One upload at a time; PHP's built-in error codes surfaced plainly.
    $file = $_FILES['firmware'] ?? null;
    if ($file === null || !is_array($file) || ($file['error'] ?? UPLOAD_ERR_NO_FILE) !== UPLOAD_ERR_OK) {
        $errors[] = 'Choose a .bin file to upload.';
    } elseif (filesize($file['tmp_name']) === false || filesize($file['tmp_name']) === 0) {
        $errors[] = 'The uploaded file is empty.';
    }

    if (empty($errors)) {
        $bytes = file_get_contents($file['tmp_name']);
        if ($bytes === false) {
            $errors[] = 'Could not read the uploaded file.';
        } else {
            $res = $relay->postBinary('/api/firmware', $bytes, $version);
            if ($res['ok'] && isset($res['body']['ok'])) {
                $notices[] = 'Firmware ' . $version . ' stored (' . ($res['body']['size'] ?? '?') . ' bytes). Nodes pick it up on their next OTA check.';
            } else {
                $bodyErrors = is_array($res['body']['errors'] ?? null) ? $res['body']['errors'] : [];
                $errors = array_merge($errors, $bodyErrors);
                if (empty($errors)) {
                    $errors[] = $res['error'] ?? 'The relay refused the upload.';
                }
            }
        }
    }
}

$fw = $relay->get('/api/firmware');
$current = ($fw['ok'] && is_array($fw['body']['current'] ?? null)) ? $fw['body']['current'] : null;

Layout::header('Firmware', 'ota');
Layout::messages($errors, $notices);

echo '<section class="card narrow">' . "\n";
echo '<h1>Firmware (OTA)</h1>' . "\n";
echo '<p class="muted">Nodes check the relay every ' . '60' . ' s and flash the served image when its version differs from theirs. Build the sketch with Arduino IDE or arduino-cli (see INSTALL.md) and upload the .bin here.</p>' . "\n";
echo '<p>Currently served: <strong>' . e(is_array($current) && isset($current['version']) ? $current['version'] : 'nothing yet — upload one below') . '</strong>';
if (is_array($current) && !empty($current['md5'])) {
    echo ' · md5 <code>' . e($current['md5']) . '</code>';
}
echo '</p>' . "\n";
echo '<form method="post" action="ota.php" enctype="multipart/form-data">' . "\n";
echo Auth::csrfField() . "\n";
echo '<label>Version <input name="version" placeholder="1.0.1" pattern="[A-Za-z0-9._-]{1,32}" required></label>' . "\n";
echo '<label>Firmware image (.bin) <input type="file" name="firmware" accept=".bin" required></label>' . "\n";
echo '<button type="submit">Upload firmware</button>' . "\n";
echo '</form>' . "\n";
echo '</section>' . "\n";

echo '<section class="card">' . "\n";
echo '<h2>What the nodes run</h2>' . "\n";
echo '<p class="muted">A node reports its firmware version in /api/status (see Devices). After uploading a new version here, watch that column change as nodes reboot through the update — the relay log and the node logs (Logs page) show each step: check, download, flash, restart.</p>' . "\n";
echo '</section>' . "\n";
Layout::footer();
