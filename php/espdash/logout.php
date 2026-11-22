<?php
// logout.php — ends the session and returns to the login page.
require __DIR__ . '/app.php';

if (($_SERVER['REQUEST_METHOD'] ?? '') === 'POST') {
    Auth::verifyCsrf();
}
Auth::logout();
header('Location: login.php');
exit;
