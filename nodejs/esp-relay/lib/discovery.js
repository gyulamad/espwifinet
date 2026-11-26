'use strict';

// discovery.js — how a node finds the relay when the address it has stored (the setup page's relay
// host) stops working, e.g. because the router gave the Pi a new address (task #160). Pure apart from
// `crypto`: no sockets (server.js owns the UDP socket), so every rule here is unit tested with plain
// `node`.
//
// The protocol is two tiny UDP datagrams — the same mechanism the camera project (espcamsys2) ships,
// retagged for this fleet; the firmware's side is arduino/ESP_node/relay_find_logic.h, and the two
// must agree byte for byte:
//
//   node  -> relay   "ESPNET1?" + <nonce>                       nonce: 16 lowercase hex characters,
//                                                                made up fresh by the node
//   relay -> node    "ESPNET1!" + <nonce> + ":" + <mac>         mac: HMAC-SHA256(nodeKey,
//                                                                "ESPNET1!" + <nonce>) as 64 hex characters
//
// Why a keyed answer instead of "send the nodeKey to whoever answers": a searching node sends its probe
// to many addresses (a sweep of its subnet, a broadcast), and most of them are not the relay. The probe
// carries no secret, and only a machine that knows nodeKey can answer it correctly, so
//   - the key is never sent to a printer, a NAS or a stranger on the network, and
//   - a machine that merely answers "yes, I'm the relay" is ignored by the node.
// The fresh nonce means an answer recorded earlier is worthless. (This does not defend against someone
// who can already sit in the middle of your LAN: the nodes' control channel is one shared fleet key,
// as before.)

const crypto = require('crypto');

const PROBE_TAG = 'ESPNET1?';
const REPLY_TAG = 'ESPNET1!';
const NONCE_RE = /^[0-9a-f]{16}$/;
const PROBE_LENGTH = PROBE_TAG.length + 16;

const DEFAULT_PORT = 8083; // the firmware's ESPNET_DISCOVERY_PORT; change both together

// The nonce of a valid probe, or null for anything else (any other datagram is silently ignored:
// the relay must not answer a port scanner, and an answer must never be bigger than a request
// by much — see buildReply).
function parseProbe(buf) {
  if (!Buffer.isBuffer(buf) || buf.length !== PROBE_LENGTH) return null;
  const text = buf.toString('latin1');
  if (!text.startsWith(PROBE_TAG)) return null;
  const nonce = text.slice(PROBE_TAG.length);
  return NONCE_RE.test(nonce) ? nonce : null;
}

function macFor(key, nonce) {
  return crypto.createHmac('sha256', key).update(REPLY_TAG + nonce).digest('hex');
}

// The answer to a valid probe. Needs a non-empty key: an unset nodeKey must not turn into an answer
// anyone could compute (same rule as server.js's requireRelayKey).
function buildReply(nonce, key) {
  if (typeof key !== 'string' || key === '' || !NONCE_RE.test(nonce)) return null;
  return Buffer.from(`${REPLY_TAG}${nonce}:${macFor(key, nonce)}`, 'latin1');
}

// What a node does with an answer (the firmware does the same in C++; this is here so the tests
// and tools can check a relay's answer independently of the firmware).
function verifyReply(buf, nonce, key) {
  if (typeof key !== 'string' || key === '' || !Buffer.isBuffer(buf)) return false;
  const expected = buildReply(nonce, key);
  if (!expected || buf.length !== expected.length) return false;
  return crypto.timingSafeEqual(buf, expected);
}

// discoveryPort from config.js / DISCOVERY_PORT: a whole number 1..65535, or 0 to switch discovery
// off. Anything else falls back to the default, with a warning the caller shows.
function resolveDiscoveryPort(value) {
  if (value === undefined || value === null || value === '') return { port: DEFAULT_PORT, warnings: [] };
  // Only a number or digits (an environment variable is text): Number(true) is 1, which would
  // quietly become port 1.
  const n = typeof value === 'number' ? value : (typeof value === 'string' && /^\d{1,5}$/.test(value) ? Number(value) : NaN);
  if (Number.isInteger(n) && n >= 0 && n <= 65535) return { port: n, warnings: [] };
  return { port: DEFAULT_PORT, warnings: [`discoveryPort ${JSON.stringify(value)} is not a port number (0-65535) — using ${DEFAULT_PORT}`] };
}

module.exports = { PROBE_TAG, REPLY_TAG, DEFAULT_PORT, parseProbe, buildReply, verifyReply, macFor, resolveDiscoveryPort };
