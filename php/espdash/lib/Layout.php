<?php
// lib/Layout.php — one place builds the page shell: <head>, nav and footer,
// plus e(), the single HTML-escaping helper every page must use for any
// value that came from the relay or the user (XSS guard).

function e($value)
{
    return htmlspecialchars((string)$value, ENT_QUOTES, 'UTF-8');
}

class Layout
{
    public static function header($title, $active)
    {
        // CSP without inline scripts/styles: the app keeps its JS and CSS in
        // assets/, so nothing can inject script through a data value.
        header("Content-Security-Policy: default-src 'self'; img-src 'self' data:; style-src 'self'; script-src 'self'");
        header('X-Content-Type-Options: nosniff');
        header('X-Frame-Options: DENY');
        header('Referrer-Policy: no-referrer');
        $nav = function ($href, $label, $key) use ($active) {
            $class = ($key === $active) ? ' class="active"' : '';
            return '<a href="' . e($href) . '"' . $class . '>' . e($label) . '</a>';
        };
        echo '<!DOCTYPE html>' . "\n";
        echo '<html lang="en">' . "\n";
        echo '<head>' . "\n";
        echo '<meta charset="utf-8">' . "\n";
        echo '<meta name="viewport" content="width=device-width, initial-scale=1">' . "\n";
        echo '<title>' . e($title) . ' · ESP Wifi Network</title>' . "\n";
        echo '<link rel="stylesheet" href="assets/style.css">' . "\n";
        echo '</head>' . "\n";
        echo '<body>' . "\n";
        echo '<header class="topbar"><div class="wrap"><span class="brand">ESP Wifi Network</span>' . "\n";
        echo '<nav>' . "\n";
        echo $nav('index.php', 'Dashboard', 'dashboard') . "\n";
        echo $nav('devices.php', 'Devices', 'devices') . "\n";
        echo $nav('logs.php', 'Logs', 'logs') . "\n";
        echo $nav('settings.php', 'Settings', 'settings') . "\n";
        echo $nav('ota.php', 'Firmware', 'ota') . "\n";
        echo '</nav>' . "\n";
        echo '<span class="user">' . e($_SESSION['user'] ?? '') . ' <a class="logout" href="logout.php">Sign out</a></span>' . "\n";
        echo '</div></header>' . "\n";
        echo '<main class="wrap">' . "\n";
    }

    public static function footer()
    {
        echo '</main>' . "\n";
        echo '<footer class="wrap foot">ESP Wifi Network · relay + ESP nodes</footer>' . "\n";
        echo '</body>' . "\n";
        echo '</html>' . "\n";
    }

    // Renders a flash-style message list (relay errors, save confirmations).
    public static function messages(array $errors, array $notices)
    {
        foreach ($errors as $msg) {
            echo '<p class="msg err">' . e($msg) . '</p>' . "\n";
        }
        foreach ($notices as $msg) {
            echo '<p class="msg ok">' . e($msg) . '</p>' . "\n";
        }
    }
}
