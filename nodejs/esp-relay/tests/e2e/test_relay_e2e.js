'use strict';

// test_relay_e2e.js — starts the real server.js on scratch ports/dirs and
// drives it like the production actors do: a simulated node (it reports the
// esp8266 chip name on the wire) over raw
// TCP (same wire protocol as arduino/ESP_node) and the PHP frontend's
// HTTP calls with the shared relay key.
//
// Run: node tests/e2e/test_relay_e2e.js  (needs `npm install` for express)

const { spawn } = require('child_process');
const net = require('net');
const fs = require('fs');
const os = require('os');
const path = require('path');
const assert = require('assert');

const ROOT = path.join(__dirname, '..', '..');
const RELAY_KEY = 'e2e-relay-key';
const NODE_KEY = 'e2e-node-key';

// ── test scaffolding ──
let server = null;
let tmpDir = null;
const failures = [];

function startRelay() {
  tmpDir = fs.mkdtempSync(path.join(os.tmpdir(), 'espwifinet-e2e-'));
  server = spawn(process.execPath, [path.join(ROOT, 'server.js')], {
    cwd: ROOT,
    env: {
      ...process.env,
      PORT: '0', // ephemeral ports; the real ones are read from the startup log
      NODE_PORT: '0',
      RELAY_KEY,
      NODE_KEY,
      DATA_DIR: path.join(tmpDir, 'data'),
      LOG_DIR: path.join(tmpDir, 'logs'),
      FIRMWARE_DIR: path.join(tmpDir, 'firmware'),
      OFFLINE_AFTER_MS: '1500',
      DISCOVERY_PORT: '0', // task #160's responder has its own e2e (test_discovery_e2e.js)
    },
    stdio: ['ignore', 'pipe', 'pipe'],
  });
  server.stderr.on('data', (d) => process.stderr.write(`[relay-err] ${d}`));
  return new Promise((resolve, reject) => {
    let out = '';
    let nodePort = null;
    let httpPort = null;
    const onData = (d) => {
      out += d.toString();
      const mNode = out.match(/\[relay\] node TCP server on :(\d+)/);
      const mHttp = out.match(/\[relay\] http api on :(\d+)/);
      if (mNode) nodePort = Number(mNode[1]);
      if (mHttp) httpPort = Number(mHttp[1]);
      if (nodePort && httpPort) {
        server.stdout.off('data', onData);
        resolve({ nodePort, httpPort });
      }
    };
    server.stdout.on('data', onData);
    server.on('exit', (code) => reject(new Error(`relay exited early (code ${code}): ${out}`)));
    setTimeout(() => reject(new Error(`relay did not report ports: ${out}`)), 10000).unref();
  });
}

function stopRelay() {
  if (server) server.kill('SIGTERM');
  if (tmpDir) fs.rmSync(tmpDir, { recursive: true, force: true });
}

async function api(httpPort, method, urlPath, body, headers = {}) {
  const isJson = body !== undefined && !Buffer.isBuffer(body);
  const res = await fetch(`http://127.0.0.1:${httpPort}${urlPath}`, {
    method,
    headers: {
      'X-API-Key': RELAY_KEY,
      ...(isJson ? { 'Content-Type': 'application/json' } : {}),
      ...headers,
    },
    body: body === undefined ? undefined : (Buffer.isBuffer(body) ? body : JSON.stringify(body)),
  });
  const text = await res.text();
  let json = null;
  try { json = JSON.parse(text); } catch (e) { /* binary/plain endpoints */ }
  return { status: res.status, json, text };
}

// A minimal node over TCP (esp8266 chip name on the wire): authenticates,
// answers pings, and records
// everything the relay sends — the same behaviour the Arduino sketch implements.
class FakeNode {
  constructor(nodePort, deviceId, apiKey = NODE_KEY) {
    this.deviceId = deviceId;
    this.apiKey = apiKey;
    this.received = [];
    this.buffer = Buffer.alloc(0);
    this.socket = new net.Socket();
    this.socket.connect(nodePort, '127.0.0.1');
    this.connected = new Promise((resolve, reject) => {
      this.socket.once('connect', resolve);
      this.socket.once('error', reject);
    });
    this.socket.on('data', (chunk) => {
      this.buffer = Buffer.concat([this.buffer, chunk]);
      for (;;) {
        if (this.buffer.length < 4) break;
        const len = this.buffer.readUInt32BE(0);
        if (this.buffer.length < 4 + len) break;
        const payload = this.buffer.slice(4, 4 + len);
        this.buffer = this.buffer.slice(4 + len);
        this.received.push(JSON.parse(payload.toString('utf8')));
      }
    });
  }

  auth() {
    this.socket.write(`${this.deviceId}\t${this.apiKey}\n`);
  }

  send(obj) {
    const json = Buffer.from(JSON.stringify(obj), 'utf8');
    const head = Buffer.alloc(4);
    head.writeUInt32BE(json.length, 0);
    this.socket.write(Buffer.concat([head, json]));
  }

  waitFor(pred, ms = 4000, desc = 'message') {
    const start = Date.now();
    return new Promise((resolve, reject) => {
      const poll = setInterval(() => {
        const found = this.received.find(pred);
        if (found) {
          clearInterval(poll);
          resolve(found);
        } else if (Date.now() - start > ms) {
          clearInterval(poll);
          reject(new Error(`timeout waiting for ${desc}; got ${JSON.stringify(this.received)}`));
        }
      }, 25);
    });
  }

  close() {
    this.socket.destroy();
  }
}

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

async function step(name, fn) {
  try {
    await fn();
    console.log(`PASS e2e: ${name}`);
  } catch (e) {
    failures.push(name);
    console.error(`FAIL e2e: ${name}: ${e && e.stack ? e.stack : e}`);
  }
}

// A plausible ESP image for the esp8266 image set (0xE9 magic + filler).
const IMAGE = Buffer.concat([Buffer.from([0xe9]), Buffer.alloc(100, 0x5a)]);

async function main() {
  const { nodePort, httpPort } = await startRelay();

  await step('http api rejects missing relay key', async () => {
    const res = await fetch(`http://127.0.0.1:${httpPort}/api/status`);
    assert.strictEqual(res.status, 401);
  });

  await step('http api rejects wrong relay key', async () => {
    const res = await fetch(`http://127.0.0.1:${httpPort}/api/status`, { headers: { 'X-API-Key': 'nope' } });
    assert.strictEqual(res.status, 401);
  });

  await step('status starts empty with gpio off', async () => {
    const { json } = await api(httpPort, 'GET', '/api/status');
    assert.strictEqual(json.desiredGpio, 'off');
    assert.strictEqual(json.gpioPin, 5); // default settings
    assert.deepStrictEqual(json.devices, []);
  });

  await step('node with wrong api key is dropped', async () => {
    const node = new FakeNode(nodePort, 'node-bad', 'wrong-key');
    await node.connected;
    node.auth();
    await sleep(300);
    const { json } = await api(httpPort, 'GET', '/api/status');
    assert.deepStrictEqual(json.devices, []);
    node.close();
  });

  await step('node auth + hello/status land in /api/status with ssid and signal', async () => {
    const node = new FakeNode(nodePort, 'node-1');
    await node.connected;
    node.auth();
    const settings = await node.waitFor((m) => m.type === 'settings', 4000, 'initial settings');
    assert.strictEqual(settings.gpioPin, 5);
    const gpioInit = await node.waitFor((m) => m.type === 'set_gpio', 4000, 'initial gpio');
    assert.strictEqual(gpioInit.state, 'off');

    node.send({ type: 'hello', fw: '1.0.0', chip: 'esp8266' });
    node.send({ type: 'status', ssid: 'HomeNet', rssi: -58, heapFree: 20000, uptimeMs: 5000, gpioPin: 5, gpioState: 'off' });

    await sleep(300);
    const { json } = await api(httpPort, 'GET', '/api/status');
    assert.strictEqual(json.devices.length, 1);
    const dev = json.devices[0];
    assert.strictEqual(dev.deviceId, 'node-1');
    assert.strictEqual(dev.online, true);
    assert.strictEqual(dev.fwVersion, '1.0.0');
    assert.strictEqual(dev.ssid, 'HomeNet');
    assert.strictEqual(dev.rssi, -58);
    assert.strictEqual(dev.signalBars, 3);
    node.close();
    await sleep(300);
  });

  await step('toggle delivers set_gpio to the online node and ack updates state', async () => {
    const node = new FakeNode(nodePort, 'node-2');
    await node.connected;
    node.auth();
    await node.waitFor((m) => m.type === 'settings');

    const { json: toggled } = await api(httpPort, 'GET', '/api/status').then(() => api(httpPort, 'POST', '/api/gpio', { state: 'on' }));
    assert.strictEqual(toggled.ok, true);
    assert.strictEqual(toggled.delivered, 1);

    const gpio = await node.waitFor((m) => m.type === 'set_gpio' && m.state === 'on', 4000, 'set_gpio on');
    assert.strictEqual(gpio.pin, 5);
    assert.strictEqual(gpio.activeHigh, true);

    node.send({ type: 'gpio_ack', state: 'on', pin: 5 });
    await sleep(200);
    const { json } = await api(httpPort, 'GET', '/api/status');
    const dev = json.devices.find((x) => x.deviceId === 'node-2');
    assert.strictEqual(dev.gpioState, 'on');
    assert.strictEqual(json.desiredGpio, 'on');
    node.close();
  });

  await step('reconnecting node receives the current desired gpio and settings', async () => {
    const node = new FakeNode(nodePort, 'node-3');
    await node.connected;
    node.auth();
    const gpio = await node.waitFor((m) => m.type === 'set_gpio', 4000, 'catch-up gpio');
    assert.strictEqual(gpio.state, 'on'); // left on by the previous step
    node.close();
  });

  await step('settings save pushes new gpio pin to online nodes', async () => {
    const node = new FakeNode(nodePort, 'node-4');
    await node.connected;
    node.auth();
    await node.waitFor((m) => m.type === 'settings');

    const { json: saved } = await api(httpPort, 'POST', '/api/settings', { gpioPin: 14, names: { 'node-4': 'Garden pump' } });
    assert.strictEqual(saved.ok, true);
    assert.strictEqual(saved.settings.gpioPin, 14);

    const settings = await node.waitFor((m) => m.type === 'settings' && m.gpioPin === 14, 4000, 'new pin');
    assert.strictEqual(settings.gpioPin, 14);

    const { json } = await api(httpPort, 'GET', '/api/status');
    assert.strictEqual(json.devices.find((x) => x.deviceId === 'node-4').name, 'Garden pump');

    const { json: bad } = await api(httpPort, 'POST', '/api/settings', { gpioPin: 99 });
    assert.strictEqual(bad.status || 200, 200); // shape check below
    assert.ok(bad.errors.some((e) => e.includes('gpioPin')));
    node.close();
  });

  await step('settings push carries the log level; /api/config summarizes the system', async () => {
    const node = new FakeNode(nodePort, 'node-loglevel');
    await node.connected;
    node.auth();
    await node.waitFor((m) => m.type === 'settings');

    const { json: saved } = await api(httpPort, 'POST', '/api/settings', { minLogLevel: 'warn' });
    assert.strictEqual(saved.ok, true);
    assert.strictEqual(saved.settings.minLogLevel, 'warn');

    // the node hears the new floor with the settings message (#36)
    const pushed = await node.waitFor((m) => m.type === 'settings' && m.minLogLevel === 'warn', 4000, 'min log level');
    assert.strictEqual(pushed.minLogLevel, 'warn');

    // system summary without secrets: only isSet booleans, plus what is
    // editable live vs what needs a relay restart
    const { json: cfg } = await api(httpPort, 'GET', '/api/config');
    assert.strictEqual(typeof cfg.nodePort, 'number');
    assert.strictEqual(typeof cfg.httpPort, 'number');
    assert.strictEqual(cfg.nodeKeySet, true);
    assert.strictEqual(cfg.relayKeySet, true);
    assert.ok(cfg.editableLive.includes('offlineAfterMs'));
    assert.ok(cfg.needsRestart.includes('config.js'));
    node.close();
  });

  await step('logs from the node reach the per-device log file and the logs api', async () => {
    const node = new FakeNode(nodePort, 'node-5');
    await node.connected;
    node.auth();
    await node.waitFor((m) => m.type === 'settings');
    node.send({ type: 'log', level: 'warn', msg: 'wifi reconnect 3' });
    await sleep(300);
    const { json } = await api(httpPort, 'GET', '/api/logs?device=node-5');
    assert.ok(json.entries.some((e) => e.msg === 'wifi reconnect 3' && e.level === 'warn'));
    // connect/disconnect lines belong to the node's log; the relay's own log
    // records settings saves
    const { json: nodeLog } = await api(httpPort, 'GET', '/api/logs?device=node-5&lines=100');
    assert.ok(nodeLog.entries.some((e) => e.msg.includes('connected from')));
    const { json: rel } = await api(httpPort, 'GET', '/api/logs?device=relay');
    assert.ok(rel.entries.some((e) => e.msg.includes('settings saved')));
    node.close();
  });

  await step('firmware upload stores image; version + bin endpoints serve it', async () => {
    const { json: bad } = await api(httpPort, 'POST', '/api/firmware?version=1.0.1', Buffer.alloc(32, 1), { 'Content-Type': 'application/octet-stream' });
    assert.strictEqual(bad.errors.length, 1); // missing 0xE9

    const { json: ok } = await api(httpPort, 'POST', '/api/firmware?version=1.0.1', IMAGE, { 'Content-Type': 'application/octet-stream' });
    assert.strictEqual(ok.ok, true);
    assert.strictEqual(ok.version, '1.0.1');

    const vres = await fetch(`http://127.0.0.1:${httpPort}/firmware/version`);
    const vtext = await vres.text();
    assert.strictEqual(vtext, '1.0.1\n' + ok.md5 + '\n');

    const bres = await fetch(`http://127.0.0.1:${httpPort}/firmware/node.bin`);
    const buf = Buffer.from(await bres.arrayBuffer());
    assert.strictEqual(buf.length, IMAGE.length);
    assert.strictEqual(buf[0], 0xe9);
  });

  await step('the settings-driven offline window marks a silent node offline', async () => {
    // #36: the window the sweep uses comes from the settings page once a
    // save happened — shorten it here through the API and watch it apply.
    const tooShort = await api(httpPort, 'POST', '/api/settings', { offlineAfterMs: 1500 });
    assert.strictEqual(tooShort.status, 400); // below the 30000 floor
    assert.ok(tooShort.json.errors.some((e) => e.includes('30000')));
    const below3x = await api(httpPort, 'POST', '/api/settings', { statusIntervalMs: 60000, offlineAfterMs: 60000 });
    assert.strictEqual(below3x.status, 400); // in bounds, but less than 3x the interval
    assert.ok(below3x.json.errors.some((e) => e.includes('3x statusIntervalMs')));

    const { json: win } = await api(httpPort, 'POST', '/api/settings', { statusIntervalMs: 10000, offlineAfterMs: 30000 });
    assert.strictEqual(win.ok, true);
    assert.strictEqual(win.settings.offlineAfterMs, 30000);

    const node = new FakeNode(nodePort, 'node-silent');
    await node.connected;
    node.auth();
    await node.waitFor((m) => m.type === 'settings');
    await sleep(34000); // 30000 window + sweep interval + margin
    const { json } = await api(httpPort, 'GET', '/api/status');
    const dev = json.devices.find((x) => x.deviceId === 'node-silent');
    assert.strictEqual(dev.online, false);
    node.close();
  });

  await step('healthz reports fleet counts', async () => {
    const res = await fetch(`http://127.0.0.1:${httpPort}/healthz`);
    const json = await res.json();
    assert.strictEqual(json.ok, true);
    assert.ok(json.nodes >= 5);
  });

  stopRelay();
  if (failures.length > 0) {
    console.error(`\n${failures.length} e2e steps FAILED`);
    process.exit(1);
  }
  console.log('\ne2e: all steps passed');
}

main().catch((e) => {
  console.error(e);
  stopRelay();
  process.exit(1);
});
