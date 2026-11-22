dashboard/
config.example.php   -> copy to config.php and edit (relay URL, relay key, users)
app.php              -> shared bootstrap: config, session, login guard, relay client
login.php/logout.php -> session login (public) and sign-out
index.php            -> dashboard: the one On/Off toggle + node status table
devices.php          -> full fleet table: SSID, signal strength, IP, fw, uptime, heap, GPIO
logs.php             -> per-device remote logs + the relay's own log
settings.php         -> GPIO pin, active level, status interval, node names
ota.php              -> firmware version + upload form
lib/Relay.php        -> the only place talking HTTP to the relay
lib/Auth.php         -> session login, CSRF tokens
lib/Layout.php       -> page shell + HTML escaping (e())
assets/style.css     -> responsive styling (CSP-safe, no inline styles)
