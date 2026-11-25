'use strict';

// server.js — the ESP Wifi Network relay, run on the Raspberry Pi.
//
// Two listeners, one process:
//   - a raw TCP server (config.nodePort): every ESP node opens ONE
//     outbound connection here, authenticates with "<deviceId>\t<apiKey>\n"
//     and then exchanges [4-byte BE length][JSON] messages. Outbound-only is
//     what lets nodes sit behind WiFi extenders and NAT routers: the node
//     dials us, so no router configuration is needed, and the same
//     connection carries our commands back to the node (set_gpio), which is
//     impossible to do from outside a NAT.
//   - an Express HTTP API (config.port) for the PHP frontend, authenticated
//     with the shared relayKey via X-API-Key. The PHP app is the only HTTP
//     client that matters (the operator's browser talks to PHP, not to us).
//
// Decision logic lives in lib/ as pure functions (unit tested under
// tests/node/); this file wires it to net/Express/fs — the split is what
// makes the relay testable.

const net = require('net');
const express = require('express');
const crypto = require('crypto');
const fs = require('fs');
const path = require('path');

// config.js is the operator's file (gitignored). For a fresh checkout/zip it
// may not exist yet: fall back to example.config.js with a loud warning so
// the test suite still runs; production installs copy example.config.js.
const baseConfig = (() => {
  try {
    return require('./config');
  } catch (e) {
    if (e.code !== 'MODULE_NOT_FOUND' || !String(e.message).includes('./config')) throw e;
    console.warn('[config] config.js not found — using example.config.js (CHANGE-ME keys); copy example.config.js to config.js and edit it');
    return require('./example.config.js');
  }
})();
const protocol = require('./lib/protocol');
const devices = require('./lib/devices');
const { SettingsStore } = require('./lib/settings');
const { LogWriter } = require('./lib/logwriter');
const gpiostate = require('./lib/gpiostate');
const firmware = require('./lib/firmware');
const { envOr, errorMessage, readJsonFile, writeFileAtomic, ensureDir, truncate } = require('./lib/util');

// ── Config: config.js is the baseline, env vars override (PORT, NODE_PORT,
// RELAY_KEY, NODE_KEY) so tests can run two relays side by side. ──
const PORT = Number(envOr(process.env, 'PORT', baseConfig.port));
const NODE_PORT = Number(envOr(process.env, 'NODE_PORT', baseConfig.nodePort));
const RELAY_KEY = envOr(process.env, 'RELAY_KEY', baseConfig.relayKey);
const NODE_KEY = envOr(process.env, 'NODE_KEY', baseConfig.nodeKey);
const DATA_DIR = envOr(process.env, 'DATA_DIR', path.join(__dirname, 'data'));
const LOG_DIR = envOr(process.env, 'LOG_DIR', path.join(__dirname, 'logs'));
const FIRMWARE_DIR = envOr(process.env, 'FIRMWARE_DIR', path.join(__dirname, 'firmware'));

// How long a node may stay silent (no status/pong) before the dashboard
// shows it offline. Nodes report every settings.statusIntervalMs, so 3x that
// covers a lost status without hiding a real outage for long.
const OFFLINE_AFTER_MS = Number(envOr(process.env, 'OFFLINE_AFTER_MS', baseConfig.offlineAfterMs));

const app = express();
// Baseline security headers first, so every response carries them.
app.use((req, res, next) => {
  res.set('X-Content-Type-Options', 'nosniff');
  res.set('Referrer-Policy', 'no-referrer');
  res.set('X-Frame-Options', 'DENY');
  next();
});
app.use(express.json({ limit: '64kb' }));
app.use(express.raw({ limit: 2 * 1024 * 1024, type: 'application/octet-stream' }));

// Every /api route requires the shared relay key. Compared in constant time
// with a length guard first so the secret's length is not leaked by timing.
function requireRelayKey(req, res, next) {
  const keyBuf = Buffer.from(req.get('X-API-Key') || '');
  const refBuf = Buffer.from(RELAY_KEY);
  if (keyBuf.length !== refBuf.length || !crypto.timingSafeEqual(keyBuf, refBuf)) {
    res.status(401).json({ error: 'unauthorized' });
    return;
  }
  next();
}
app.use('/api', requireRelayKey);

ensureDir(DATA_DIR);
ensureDir(LOG_DIR);
ensureDir(FIRMWARE_DIR);

const settingsStore = new SettingsStore(path.join(DATA_DIR, 'settings.json'), readJsonFile);
for (const w of settingsStore.warnings) console.warn(`[settings] ${w}`);

const logs = new LogWriter(LOG_DIR);
const logsWarning = (msg) => logs.append({ level: 'warn', device: 'relay', msg });
const logsError = (msg) => logs.append({ level: 'error', device: 'relay', msg });

// ── Fleet state (memory) + persisted desired state (disk) ──
const connected = new Map(); // deviceId -> net.Socket of the live connection
const fleet = new Map(); // deviceId -> device info record (survives disconnects)
const GPIO_STATE_FILE = path.join(DATA_DIR, 'state.json');
let desiredGpio = gpiostate.loadGpioState(GPIO_STATE_FILE, readJsonFile);

function deviceRecord(deviceId) {
  let dev = fleet.get(deviceId);
  if (!dev) {
    dev = devices.createDeviceInfo(deviceId, Date.now());
    fleet.set(deviceId, dev);
  }
  return dev;
}

function markSeen(deviceId) {
  const dev = fleet.get(deviceId);
  if (dev) dev.lastSeen = Date.now();
}

// Sends one message object to a node's connection. Returns false when the
// socket is gone or the message could not be encoded (never throws into the
// caller's loop).
function sendToNode(deviceId, obj) {
  const sock = connected.get(deviceId);
  if (!sock || sock.destroyed) return false;
  const buf = protocol.encodeMessage(obj);
  if (!buf) {
    logsError(`could not encode message for ${deviceId} (type ${obj && obj.type})`);
    return false;
  }
  sock.write(buf);
  return true;
}

// The settings payload every node needs right after auth (and whenever the
// settings page saves): the GPIO pin, the active level, its status cadence
// and the lowest log level the node queues (#36 — all of it settings-page
// editable now).
function settingsMessage() {
  const s = settingsStore.get();
  return {
    type: 'settings',
    gpioPin: s.gpioPin,
    gpioActiveHigh: s.gpioActiveHigh,
    statusIntervalMs: s.statusIntervalMs,
    minLogLevel: s.minLogLevel,
  };
}

// The desired GPIO state travels as its own message: nodes apply it on hello
// (so a node that missed a toggle while offline catches up) and on every save.
function gpioMessage() {
  const s = settingsStore.get();
  return { type: 'set_gpio', state: desiredGpio, pin: s.gpioPin, activeHigh: s.gpioActiveHigh };
}

// Pushes settings + desired GPIO to one node (used on connect and on save).
function pushConfig(deviceId) {
  sendToNode(deviceId, settingsMessage());
  sendToNode(deviceId, gpioMessage());
}

// ── Node TCP server ──
const nodeServer = net.createServer((socket) => {
  socket.setNoDelay(true);
  let authed = false;
  let deviceId = null;
  let buffer = Buffer.alloc(0);

  socket.on('error', () => { /* ECONNRESET etc. — the close handler cleans up */ });

  socket.on('close', () => {
    if (!authed) return; // never authed: nothing registered
    // Only clean up if this socket is still the registered one: when a node
    // reconnects we destroy the old socket first, and the old socket's close
    // event must not kick out the new connection (it fires asynchronously).
    if (connected.get(deviceId) !== socket) return;
    connected.delete(deviceId);
    const dev = fleet.get(deviceId);
    if (dev) {
      dev.online = false;
      dev.lastDisconnectReason = 'socket closed';
    }
    logs.append({ level: 'info', device: deviceId, msg: 'disconnected' });
  });

  socket.on('data', (chunk) => {
    buffer = Buffer.concat([buffer, chunk]);

    if (!authed) {
      // ── Phase 1: the one-line auth handshake ──
      const lineEnd = protocol.findAuthLineEnd(buffer);
      if (lineEnd === -1) {
        if (protocol.isAuthLineTooLong(buffer.length)) {
          logsWarning('dropped connection: auth line too long');
          socket.destroy();
        }
        return; // wait for the rest of the line
      }
      const parsed = protocol.parseAuthLine(buffer.slice(0, lineEnd));
      buffer = buffer.slice(lineEnd + 1);
      if (!parsed || !protocol.isDeviceIdValid(parsed.deviceId) || parsed.apiKey !== NODE_KEY) {
        logsWarning('rejected connection: bad auth line or unknown key');
        socket.destroy();
        return;
      }
      deviceId = parsed.deviceId;
      authed = true;

      // Two senders on one id: decide which connection is the real board and
      // keep exactly one. The rules are in devices.js (unit tested); see the
      // header there for 'conflict' vs 'stale'.
      const previous = connected.get(deviceId);
      if (previous && !previous.destroyed) {
        const dev = fleet.get(deviceId);
        const silentMs = dev ? Date.now() - dev.lastSeen : 0;
        const kind = devices.isConnectionConflict(silentMs) ? 'conflict' : 'stale';
        logs.append({
          level: kind === 'conflict' ? 'warn' : 'info',
          device: deviceId,
          msg: `replacing ${kind} previous connection`,
        });
        if (kind === 'conflict') logsWarning(`two live senders claim device id ${deviceId} — check for a duplicated id in the sketch config`);
        previous.destroy();
      }

      connected.set(deviceId, socket);
      const dev = deviceRecord(deviceId);
      dev.online = true;
      dev.connectedAt = Date.now();
      dev.connectCount += 1;
      dev.lastSeen = Date.now();
      dev.ip = socket.remoteAddress || null;
      logs.append({ level: 'info', device: deviceId, msg: `connected from ${dev.ip}` });

      // Everything a (re)connecting node must know, in order: its settings,
      // then the current desired GPIO state.
      pushConfig(deviceId);
      return;
    }

    // ── Phase 2: authenticated — drain length-prefixed messages ──
    let drained;
    try {
      drained = protocol.drainMessages(buffer);
    } catch (e) {
      logs.append({ level: 'warn', device: deviceId, msg: `protocol error: ${errorMessage(e)}` });
      socket.destroy();
      return;
    }
    buffer = drained.rest;
    for (const payload of drained.messages) {
      const msg = protocol.classifyMessage(payload);
      if (!msg) {
        const dev = fleet.get(deviceId);
        if (dev) dev.droppedMessages += 1;
        logs.append({ level: 'warn', device: deviceId, msg: 'dropped malformed/unknown message' });
        continue;
      }
      handleNodeMessage(deviceId, msg);
      markSeen(deviceId);
    }
    // Bound the leftover buffer: a node streaming garbage without length
    // prefixes would otherwise grow it forever.
    if (buffer.length > protocol.MAX_MESSAGE_BYTES) {
      logs.append({ level: 'warn', device: deviceId, msg: 'unbuffered partial message overflow' });
      socket.destroy();
    }
  });

  function handleNodeMessage(id, msg) {
    const dev = fleet.get(id) || deviceRecord(id);
    switch (msg.type) {
      case 'hello':
        devices.applyHelloMessage(dev, msg);
        logs.append({ level: 'info', device: id, msg: `hello: fw=${dev.fwVersion} chip=${dev.chip}` });
        break;
      case 'status':
        devices.applyStatusMessage(dev, msg);
        break;
      case 'log': {
        const level = msg.level === 'error' || msg.level === 'warn' ? msg.level : 'info';
        logs.append({ level, device: id, msg: truncate(String(msg.msg || ''), 160) });
        break;
      }
      case 'gpio_ack':
        dev.gpioState = msg.state === 'on' ? 'on' : 'off';
        dev.gpioPin = Number.isInteger(msg.pin) ? msg.pin : dev.gpioPin;
        break;
      case 'pong':
        break; // markSeen already covered it
      default:
        break; // classifyMessage only lets known types through
    }
  }
});

// ── Liveness sweep: flip records to offline when a node stopped speaking ──
// The window is settings-driven since #36 (settings.offlineAfterMs, floor:
// 3x the status interval). Until the operator saves settings for the first
// time, config.js/env seeds it (that is also how the e2e suite shortens the
// window with OFFLINE_AFTER_MS).
function offlineAfterMs() {
  if (!settingsStore.existed) return OFFLINE_AFTER_MS;
  const s = settingsStore.get();
  return Number.isInteger(s.offlineAfterMs) && s.offlineAfterMs >= 30000 ? s.offlineAfterMs : OFFLINE_AFTER_MS;
}

setInterval(() => {
  const now = Date.now();
  const windowMs = offlineAfterMs();
  for (const dev of fleet.values()) {
    if (dev.online && now - dev.lastSeen >= windowMs) {
      dev.online = false;
      dev.lastDisconnectReason = 'silent';
      const sock = connected.get(dev.deviceId);
      if (sock && !sock.destroyed) sock.destroy(); // force the node to redial a dead link
      logs.append({ level: 'warn', device: dev.deviceId, msg: 'marked offline: no messages' });
    }
  }
}, Math.min(OFFLINE_AFTER_MS, 15000));

// Ping nodes that are connected but quiet, so half-open TCP links (typical
// behind NAT routers that dropped the mapping) surface as reconnections
// instead of zombie "online" rows.
const PING_INTERVAL_MS = 15000;
setInterval(() => {
  const now = Date.now();
  for (const [id, sock] of connected.entries()) {
    if (sock.destroyed) continue;
    const dev = fleet.get(id);
    if (dev && now - dev.lastSeen > PING_INTERVAL_MS) sendToNode(id, { type: 'ping' });
  }
}, PING_INTERVAL_MS);

// ── HTTP API for the PHP frontend ──
// (the middleware and the key check above already cover every /api route;
// the duplicated block that used to sit here was dead code — #36 tidy-up)

// GET /api/status — everything the dashboard needs in one call.
app.get('/api/status', (req, res) => {
  const s = settingsStore.get();
  res.json({
    now: Date.now(),
    desiredGpio,
    gpioPin: s.gpioPin,
    gpioActiveHigh: s.gpioActiveHigh,
    offlineAfterMs: offlineAfterMs(),
    devices: devices.buildSnapshot(fleet, (id) => settingsStore.nameFor(id), Date.now(), offlineAfterMs()),
  });
});

// POST /api/gpio { state: "on" | "off" } — the one toggle.
app.post('/api/gpio', (req, res) => {
  const state = req.body && req.body.state;
  if (!gpiostate.isGpioStateValid(state)) {
    res.status(400).json({ error: `state must be one of: ${gpiostate.GPIO_STATES.join(', ')}` });
    return;
  }
  desiredGpio = state;
  const saved = gpiostate.saveGpioState(GPIO_STATE_FILE, state, readJsonFile);
  if (!saved.ok) {
    logsError(`could not persist gpio state: ${saved.errors.join('; ')}`);
  }
  // Broadcast to every live node now; offline ones get it on reconnect.
  let delivered = 0;
  for (const id of connected.keys()) {
    if (sendToNode(id, gpioMessage())) delivered += 1;
  }
  logs.append({ level: 'info', device: 'relay', msg: `gpio -> ${state} (delivered to ${delivered} node(s))` });
  res.json({ ok: true, state, delivered });
});

// GET /api/settings, POST /api/settings — the settings page.
app.get('/api/settings', (req, res) => {
  res.json({ settings: settingsStore.get() });
});

app.post('/api/settings', (req, res) => {
  const result = settingsStore.save(req.body);
  if (!result.ok) {
    res.status(400).json({ errors: result.errors });
    return;
  }
  for (const w of result.warnings || []) logsWarning(`settings saved with warning: ${w}`);
  logs.append({ level: 'info', device: 'relay', msg: 'settings saved' });
  // Re-push the new config to every live node immediately.
  for (const id of connected.keys()) pushConfig(id);
  res.json({ ok: true, settings: result.value, warnings: result.warnings || [] });
});

// GET /api/config — the settings page's SYSTEM summary (#36): every relay
// value that used to live only in config.js, shown without ever sending a
// secret (only "is it set" booleans). Ports and keys
// are startup settings read from config.js/env on the Pi: editing them
// needs a relay restart, so they are displayed read-only instead of edited.
app.get('/api/config', (req, res) => {
  const s = settingsStore.get();
  res.json({
    now: Date.now(),
    httpPort: PORT,
    nodePort: NODE_PORT,
    nodeKeySet: NODE_KEY.length > 0 && !NODE_KEY.startsWith('CHANGE-ME'),
    relayKeySet: RELAY_KEY.length > 0 && !RELAY_KEY.startsWith('CHANGE-ME'),
    firmware: firmware.currentFirmware(FIRMWARE_DIR),
    offlineAfterMs: s.offlineAfterMs,
    statusIntervalMs: s.statusIntervalMs,
    minLogLevel: s.minLogLevel,
    editableLive: ['gpioPin', 'gpioActiveHigh', 'statusIntervalMs', 'offlineAfterMs', 'minLogLevel', 'names'],
    needsRestart: 'ports and keys are read from config.js on the Pi at startup — edit them there and restart the relay',
  });
});

// GET /api/logs?device=<id>&lines=<n> — the logs page.
app.get('/api/logs', (req, res) => {
  const deviceId = String(req.query.device || 'relay');
  const lines = Math.min(Math.max(Number(req.query.lines) || 200, 1), 1000);
  if (deviceId !== 'relay' && !protocol.isDeviceIdValid(deviceId)) {
    res.status(400).json({ error: 'invalid device id' });
    return;
  }
  res.json({ device: deviceId, entries: logs.tail(deviceId, lines) });
});

// GET /api/logs/devices — for the logs page's device selector.
app.get('/api/logs/devices', (req, res) => {
  res.json({ devices: logs.listDevices() });
});

// GET /api/firmware — what the OTA page shows, per board (#131): the two
// chips' image sets live side by side. `current` keeps meaning the esp8266
// set so an existing dashboard/API consumer reads the same shape as before.
app.get('/api/firmware', (req, res) => {
  res.json({
    esp8266: firmware.currentFirmware(FIRMWARE_DIR, 'esp8266'),
    esp32: firmware.currentFirmware(FIRMWARE_DIR, 'esp32'),
    current: firmware.currentFirmware(FIRMWARE_DIR, 'esp8266'),
  });
});

// POST /api/firmware — the OTA page's upload. The PHP client sends the raw
// image as application/octet-stream with ?version=1.2.3 (multipart would add
// a dependency for no benefit between two servers).
app.post('/api/firmware', (req, res) => {
  if (!req.body || !Buffer.isBuffer(req.body) || req.body.length === 0) {
    res.status(400).json({ errors: ['empty body — send the raw .bin as application/octet-stream'] });
    return;
  }
  const chip = String(req.query.chip || 'esp8266');
  const result = firmware.storeFirmware(FIRMWARE_DIR, req.body, String(req.query.version || ''), writeFileAtomic, chip);
  if (!result.ok) {
    res.status(400).json({ errors: result.errors });
    return;
  }
  logs.append({ level: 'info', device: 'relay', msg: `firmware ${result.version} stored for ${chip} (${result.size} bytes, md5 ${result.md5})` });
  res.json({ ok: true, chip, ...result });
});

// Node-facing OTA endpoints (the node knows RELAY_HOST:PORT already):
//   GET /firmware/version?chip=<chip> -> "<version>\n<md5>\n"
//   GET /firmware/node.bin            (the esp8266 image)
//   GET /firmware/node-esp32.bin      (the esp32 image)
// The chip-less URLs keep answering the esp8266 set so the nodes deployed
// before the per-board split keep updating unchanged; an ESP32 node asks
// with its chip (board.h's ESPNET_CHIP_NAME rides in the query) and
// downloads its own image path (board.h's ESPNET_FIRMWARE_PATH).
// These are unauthenticated but read-only and useless without network access
// to the relay.
app.get('/firmware/version', (req, res) => {
  const chip = String(req.query.chip || 'esp8266');
  if (!firmware.isKnownChip(chip)) {
    res.status(404).type('text/plain').send('unknown chip\n');
    return;
  }
  res.type('text/plain').send(firmware.versionReply(FIRMWARE_DIR, chip));
});
app.get('/firmware/node.bin', (req, res) => {
  res.setHeader('X-Content-Type-Options', 'nosniff');
  res.sendFile(path.join(FIRMWARE_DIR, firmware.imageFileName('esp8266')), (err) => {
    if (err && !res.headersSent) res.status(404).end('no firmware uploaded');
  });
});
app.get('/firmware/node-esp32.bin', (req, res) => {
  res.setHeader('X-Content-Type-Options', 'nosniff');
  res.sendFile(path.join(FIRMWARE_DIR, firmware.imageFileName('esp32')), (err) => {
    if (err && !res.headersSent) res.status(404).end('no firmware uploaded');
  });
});

// Basic health endpoint for systemd / monitoring.
app.get('/healthz', (req, res) => {
  res.json({ ok: true, nodes: fleet.size, connected: connected.size, desiredGpio });
});

// JSON error handler last: API callers always get JSON, never an HTML stack.
// eslint-disable-next-line no-unused-vars
app.use((err, req, res, next) => {
  logsError(`http error: ${errorMessage(err)}`);
  if (res.headersSent) return;
  res.status(err.status || 500).json({ error: errorMessage(err) });
});

nodeServer.listen(NODE_PORT, () => {
  console.log(`[relay] node TCP server on :${nodeServer.address().port}`);
});
const httpServer = app.listen(PORT, () => {
  console.log(`[relay] http api on :${httpServer.address().port}`);
  for (const w of settingsStore.warnings) console.warn(`[settings] ${w}`);
});
