<?php
// setup.php — the INITIAL SETUP PAGE (task #36), adapted from espcamsys2's
// safe-credentials bootstrap. A fresh install lands here instead of a raw
// "config.php is missing" error: it walks the operator through the relay
// address, the relay key and the first dashboard login, checks the relay
// really answers with the given key, and then writes config.php itself.
//
// What it borrows from espcamsys2 (php/cameras/auth.php + config_file.php):
//   - unsafe values are refused, never stored: empty username, a password
//     shorter than 8 characters, the template's CHANGE-ME keys;
//   - config.php is written ATOMICALLY (temp file + rename, no .bak ever
//     left in the web folder — a half-written config must never exist);
//   - once a valid config.php exists this page locks itself out (it only
//     ever runs BEFORE the first login exists).
//
// The page is deliberately standalone: it must work when config.php does
// not exist yet, so it cannot boot through app.php. It pulls in only
// lib/Layout.php for e() and the page shell.

declare(strict_types=1);

require __DIR__ . '/lib/Layout.php';

session_name('espdash-setup');
session_set_cookie_params([
    'lifetime' => 0,
    'path' => '/',
    'httponly' => true,
    'samesite' => 'Strict',
    'secure' => (($_SERVER['HTTPS'] ?? '') === 'on' || ($_SERVER['SERVER_PORT'] ?? '') === '443'),
]);
session_start();

// ── what shape the install is in ──
// Returns [state, reasons]: state is 'missing' (no config.php), 'template'
// (exists but still carries example/unsafe values) or 'ok'.
function dash_setup_state(string $dir): array
{
    $file = $dir . '/config.php';
    if (!file_exists($file)) {
        return ['missing', []];
    }
    $config = require $file;
    if (!is_array($config)) {
        return ['template', ['config.php does not return an array.']];
    }
    $reasons = [];
    if (!isset($config['relay_url'], $config['relay_key'], $config['users'], $config['session_name'])) {
        $reasons[] = 'config.php is missing one of the required keys (relay_url, relay_key, users, session_name).';
    }
    if (!is_array($config['users'] ?? null) || count($config['users']) === 0) {
        $reasons[] = 'no dashboard login exists yet.';
    } else {
        foreach ($config['users'] as $user => $hash) {
            if (!is_string($hash) || $hash === '' || strpos($hash, 'REPLACE-WITH') === 0) {
                $reasons[] = "the password hash for \"$user\" is still the template placeholder.";
            }
        }
    }
    if (isset($config['relay_key']) && (strpos((string)$config['relay_key'], 'CHANGE-ME') === 0)) {
        $reasons[] = 'relay_key still contains the example key.';
    }
    return (count($reasons) > 0) ? ['template', $reasons] : ['ok', []];
}

// Probes the relay: GET /api/status with the key. Classifies the failure so
// the operator sees WHAT is wrong (address, key, or nothing listening).
function dash_probe_relay(string $url, string $key): array
{
    $context = stream_context_create([
        'http' => [
            'method' => 'GET',
            'header' => "X-API-Key: $key\r\n",
            'timeout' => 5,
            'ignore_errors' => true,
        ],
    ]);
    $body = @file_get_contents(rtrim($url, '/') . '/api/status', false, $context);
    if ($body === false) {
        return [false, 'the relay did not answer — is nodejs/esp-relay running on that machine/port?'];
    }
    $status = 0;
    foreach ($http_response_header ?? [] as $h) {
        if (preg_match('#^HTTP/\S+\s+(\d+)#', $h, $m)) {
            $status = (int)$m[1];
        }
    }
    if ($status === 401) {
        return [false, 'the relay answered but rejected the key — check relay_key against relayKey in the relay\'s config.js.'];
    }
    $decoded = json_decode((string)$body, true);
    if ($status !== 200 || !is_array($decoded) || !isset($decoded['devices'])) {
        return [false, "the relay answered with HTTP $status but not its status API — is that URL the esp-relay?"];
    }
    return [true, 'relay reachable, key accepted (' . count($decoded['devices']) . ' device record(s) known).'];
}

// Atomic config write: temp file in the same folder, fsync-ed, renamed. No
// .bak copy is ever left in the web folder (the espcamsys2 rule).
function dash_write_config(string $dir, array $config): array
{
    $file = $dir . '/config.php';
    $tmp = $dir . '/.config.php.tmp-' . bin2hex(random_bytes(4));
    $php = "<?php\n// Written by setup.php — edit values by hand afterwards if needed.\n\nreturn " . var_export($config, true) . ";\n";
    $fh = @fopen($tmp, 'x');
    if ($fh === false) {
        return [false, 'could not create a temporary file in ' . $dir . ' — check the web server\'s write permission.'];
    }
    fwrite($fh, $php);
    fflush($fh);
    fclose($fh);
    if (!@rename($tmp, $file)) {
        @unlink($tmp);
        return [false, 'could not put config.php in place — check the web server\'s write permission.'];
    }
    @chmod($file, 0640);
    return [true, ''];
}

$errors = [];
$notices = [];

// The posted values survive a failed save so the operator fixes instead of retyping.
$relayUrl = '';
$username = '';
$probeMsg = '';

[$state, $stateReasons] = dash_setup_state(__DIR__);
if ($state === 'ok') {
    header('Location: index.php');
    exit;
}

if (($_SERVER['REQUEST_METHOD'] ?? '') === 'POST') {
    $postedCsrf = $_POST['csrf'] ?? '';
    if (!is_string($postedCsrf) || $postedCsrf === '' || !isset($_SESSION['setup_csrf'])
        || !hash_equals($_SESSION['setup_csrf'], $postedCsrf)) {
        http_response_code(400);
        exit('invalid CSRF token');
    }

    $relayUrl = trim((string)($_POST['relay_url'] ?? ''));
    $relayKey = (string)($_POST['relay_key'] ?? '');
    $username = trim((string)($_POST['username'] ?? ''));
    $password = (string)($_POST['password'] ?? '');
    $confirm = (string)($_POST['password_confirm'] ?? '');

    // the espcamsys2 credential rules, carried over
    if (!preg_match('#^https?://[A-Za-z0-9._~:/\[\]@!$&\'()*+,;=%-]+$#', $relayUrl)) {
        $errors[] = 'relay_url must look like http://127.0.0.1:8080 (no trailing slash).';
    }
    if ($relayKey === '' || strlen($relayKey) < 8) {
        $errors[] = 'relay_key must be at least 8 characters.';
    } elseif (strpos($relayKey, 'CHANGE-ME') === 0) {
        $errors[] = 'relay_key still is the example value — generate one (node -e "console.log(require(\'crypto\').randomBytes(24).toString(\'hex\'))").';
    }
    if ($username === '' || !preg_match('/^[A-Za-z0-9._-]{1,32}$/', $username)) {
        $errors[] = 'the dashboard username must be 1-32 characters: letters, digits, dot, dash, underscore.';
    }
    if (strlen($password) < 8) {
        $errors[] = 'the dashboard password must be at least 8 characters.';
    } elseif ($password === $username) {
        $errors[] = 'the dashboard password must not be the username.';
    } elseif ($password !== $confirm) {
        $errors[] = 'the two passwords do not match.';
    }

    if (count($errors) === 0) {
        [$probeOk, $probeMsg] = dash_probe_relay($relayUrl, $relayKey);
        if (!$probeOk) {
            $errors[] = 'Relay check failed: ' . $probeMsg . ' You can still save (the relay may be started later), or fix the values first.';
        } else {
            $notices[] = $probeMsg;
        }
    }

    if (count($errors) === 0) {
        $config = [
            'relay_url' => rtrim($relayUrl, '/'),
            'relay_key' => $relayKey,
            'users' => [$username => password_hash($password, PASSWORD_DEFAULT)],
            'session_name' => 'espdash',
        ];
        [($written), $writeError] = dash_write_config(__DIR__, $config);
        if ($written === false) {
            $errors[] = $writeError;
        } else {
            Layout::header('Setup complete', 'setup');
            echo '<section class="card narrow">' . "\n";
            echo '<h1>Setup complete</h1>' . "\n";
            if ($probeMsg !== '') {
                echo '<p class="msg ok">' . e($probeMsg) . '</p>' . "\n";
            }
            echo '<p>config.php is written for <code>' . e($relayUrl) . '</code> and the first login <strong>' . e($username) . '</strong> is created.</p>' . "\n";
            echo '<p><a href="login.php">Go to the dashboard login</a></p>' . "\n";
            echo '<p class="muted">This setup page is now locked (it only runs before the first login exists). Keep a copy of the password — there is no reset link.</p>' . "\n";
            echo '</section>' . "\n";
            Layout::footer();
            exit;
        }
    }
} else {
    // the CSRF token for the form below
    $_SESSION['setup_csrf'] = bin2hex(random_bytes(32));
    $relayUrl = 'http://127.0.0.1:8080';
}

Layout::header('Initial setup', 'setup');
echo '<section class="card narrow">' . "\n";
echo '<h1>Initial setup</h1>' . "\n";
echo '<p class="muted">First run: this page creates config.php (relay address, relay key, first dashboard login) so no config file has to be edited by hand.</p>' . "\n";
if ($state === 'template') {
    echo '<p class="msg err">The existing config.php is not usable yet:</p><ul>';
    foreach ($stateReasons as $reason) {
        echo '<li>' . e($reason) . '</li>';
    }
    echo '</ul>' . "\n";
    echo '<p class="muted">Saving below replaces it with a complete one.</p>' . "\n";
}
Layout::messages($errors, $notices);
echo '<form method="post" action="setup.php">' . "\n";
echo '<input type="hidden" name="csrf" value="' . e($_SESSION['setup_csrf']) . '">' . "\n";
echo '<fieldset>' . "\n";
echo '<legend>Relay</legend>' . "\n";
echo '<label>Relay URL (HTTP API, no trailing slash) <input name="relay_url" value="' . e($relayUrl) . '" required></label>' . "\n";
echo '<label>Relay key (relayKey in the relay\'s config.js) <input name="relay_key" type="password" autocomplete="off" required minlength="8"></label>' . "\n";
echo '<p class="muted">The key is checked against the relay before anything is saved.</p>' . "\n";
echo '</fieldset>' . "\n";
echo '<fieldset>' . "\n";
echo '<legend>Dashboard login</legend>' . "\n";
echo '<label>Username <input name="username" value="' . e($username) . '" required maxlength="32"></label>' . "\n";
echo '<label>Password (min 8 characters) <input name="password" type="password" autocomplete="new-password" required minlength="8"></label>' . "\n";
echo '<label>Repeat password <input name="password_confirm" type="password" autocomplete="new-password" required minlength="8"></label>' . "\n";
echo '</fieldset>' . "\n";
echo '<button type="submit">Save and finish setup</button>' . "\n";
echo '</form>' . "\n";
echo '</section>' . "\n";
Layout::footer();
