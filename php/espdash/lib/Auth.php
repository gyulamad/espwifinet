<?php
// lib/Auth.php — dashboard session, login and CSRF. Every POST form goes
// through verifyCsrf(); every protected page starts with requireLogin().

class Auth
{
    // Boots the session once per request with hardened cookie flags. Called
    // from app.php before anything renders.
    public static function start($sessionName)
    {
        if (session_status() === PHP_SESSION_ACTIVE) {
            return;
        }
        session_name($sessionName);
        session_set_cookie_params([
            'lifetime' => 0,
            'path' => '/',
            'httponly' => true,
            'samesite' => 'Strict',
            // 'secure' is enabled automatically when the dashboard is served
            // over HTTPS; a plain-HTTP install must not set it.
            'secure' => (($_SERVER['HTTPS'] ?? '') === 'on' || ($_SERVER['SERVER_PORT'] ?? '') === '443'),
        ]);
        session_start();
    }

    public static function isLoggedIn()
    {
        return isset($_SESSION['user']) && is_string($_SESSION['user']) && $_SESSION['user'] !== '';
    }

    // Checks the posted credentials against config. Returns true on success
    // (and regenerates the session id to stop fixation); false otherwise
    // (with one generic delay so bad usernames and bad passwords look alike).
    public static function login($username, $password, $users)
    {
        if (!is_string($username) || !is_string($password)) {
            return false;
        }
        $hash = isset($users[$username]) ? $users[$username] : '';
        $valid = is_string($hash) && $hash !== '' && password_verify($password, $hash);
        if (!$valid) {
            usleep(300000); // flatten timing differences between unknown user / bad password
            return false;
        }
        session_regenerate_id(true);
        $_SESSION['user'] = $username;
        return true;
    }

    public static function logout()
    {
        $_SESSION = [];
        if (ini_get('session.use_cookies')) {
            $p = session_get_cookie_params();
            setcookie(session_name(), '', time() - 42000, $p['path'], $p['domain'] ?? '', (bool)($p['secure'] ?? false), true);
        }
        session_destroy();
    }

    public static function requireLogin()
    {
        if (self::isLoggedIn()) {
            return;
        }
        header('Location: login.php');
        exit;
    }

    // ── CSRF: one token per session, checked on every POST ──
    public static function csrfToken()
    {
        if (!isset($_SESSION['csrf']) || !is_string($_SESSION['csrf'])) {
            $_SESSION['csrf'] = bin2hex(random_bytes(32));
        }
        return $_SESSION['csrf'];
    }

    public static function csrfField()
    {
        return '<input type="hidden" name="csrf" value="' . e(self::csrfToken()) . '">';
    }

    // Verifies the posted token; exits the request with a 400 when it is
    // missing or wrong (never falls through to the action).
    public static function verifyCsrf()
    {
        $posted = $_POST['csrf'] ?? '';
        $known = self::csrfToken();
        if (!is_string($posted) || !hash_equals($known, $posted)) {
            http_response_code(400);
            exit('invalid CSRF token');
        }
    }
}
