<?php
// lib/Relay.php — the only place that talks HTTP to the Node relay. Every
// page gets its data through this client, so timeouts, the shared key and
// error shapes are handled once (DRY).

class Relay
{
    private $baseUrl;
    private $key;

    public function __construct($baseUrl, $key)
    {
        $this->baseUrl = rtrim($baseUrl, '/');
        $this->key = $key;
    }

    // GET a JSON endpoint. Returns ['ok', 'status', 'body'] where body is the
    // decoded array (or null when the relay answered non-JSON / unreachable).
    public function get($path)
    {
        return $this->request('GET', $path, null, null);
    }

    // POST a JSON payload (settings save, gpio toggle).
    public function postJson($path, $data)
    {
        return $this->request('POST', $path, json_encode($data), 'application/json');
    }

    // POST raw binary (firmware upload) with a ?version= query parameter.
    public function postBinary($path, $bytes, $version, $chip = 'esp8266')
    {
        // #131: ?chip= picks the board's image set on the relay (esp8266
        // default, esp32) so a mixed fleet never flashes another board's bin.
        $sep = (strpos($path, '?') === false) ? '?' : '&';
        return $this->request('POST', $path . $sep . 'version=' . rawurlencode($version) . '&chip=' . rawurlencode($chip), $bytes, 'application/octet-stream');
    }

    // Low-level call: 5 s timeouts keep a hung relay from hanging the page;
    // errors are normalised into ['ok' => false, 'error' => ...] instead of
    // warnings leaking into the HTML.
    private function request($method, $path, $body, $contentType)
    {
        $ch = curl_init($this->baseUrl . $path);
        if ($ch === false) {
            return ['ok' => false, 'error' => 'curl init failed'];
        }
        $headers = ['X-API-Key: ' . $this->key];
        if ($contentType !== null) {
            $headers[] = 'Content-Type: ' . $contentType;
        }
        curl_setopt_array($ch, [
            CURLOPT_CUSTOMREQUEST => $method,
            CURLOPT_HTTPHEADER => $headers,
            CURLOPT_RETURNTRANSFER => true,
            CURLOPT_TIMEOUT => 5,
            CURLOPT_CONNECTTIMEOUT => 2,
        ]);
        if ($body !== null) {
            curl_setopt($ch, CURLOPT_POSTFIELDS, $body);
        }
        $text = curl_exec($ch);
        if ($text === false) {
            $err = curl_error($ch);
            curl_close($ch);
            return ['ok' => false, 'error' => 'relay unreachable: ' . $err];
        }
        $status = curl_getinfo($ch, CURLINFO_HTTP_CODE);
        curl_close($ch);

        $decoded = json_decode($text, true);
        if (json_last_error() !== JSON_ERROR_NONE) {
            return ['ok' => ($status >= 200 && $status < 300), 'status' => $status, 'body' => null, 'raw' => $text];
        }
        $ok = ($status >= 200 && $status < 300);
        // The relay signals business failures with 4xx + {errors:[...]}.
        return ['ok' => $ok, 'status' => $status, 'body' => $decoded];
    }
}
