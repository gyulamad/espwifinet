<?php
// login.php — the dashboard's only public page.
require __DIR__ . '/app.php';

$errors = [];

if (($_SERVER['REQUEST_METHOD'] ?? '') === 'POST') {
    Auth::verifyCsrf();
    $username = $_POST['username'] ?? '';
    $password = $_POST['password'] ?? '';
    if (Auth::login($username, $password, configValue('users'))) {
        header('Location: index.php');
        exit;
    }
    $errors[] = 'Wrong username or password.';
}

Layout::header('Sign in', 'login');
echo '<section class="card narrow">' . "\n";
echo '<h1>Sign in</h1>' . "\n";
Layout::messages($errors, []);
echo '<form method="post" action="login.php">' . "\n";
echo Auth::csrfField() . "\n";
echo '<label>Username <input name="username" autofocus autocomplete="username" required></label>' . "\n";
echo '<label>Password <input name="password" type="password" autocomplete="current-password" required></label>' . "\n";
echo '<button type="submit">Sign in</button>' . "\n";
echo '</form>' . "\n";
echo '</section>' . "\n";
Layout::footer();
