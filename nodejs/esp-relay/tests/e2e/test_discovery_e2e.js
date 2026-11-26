#!/usr/bin/env node
'use strict';
// test_discovery_e2e.js — end-to-end check that a node can find the relay
// again by itself (task #160): a REAL `server.js` process answers on UDP,
// and the question is asked both with raw datagrams and by the firmware's
// REAL search code (arduino/ESP_node/relay_find_logic.h, compiled into a
// desktop program by tests/cpp/relay_find_tool.cpp), so the C++ and the
// JavaScript are proven to agree byte for byte over real sockets. Checks:
//   - the relay answers a valid probe with a MAC only the holder of nodeKey can make, and says so in its log
//   - anything else sent to the port (junk, a probe with a bad nonce, a probe that is too long) gets NO answer
//   - the firmware search finds the relay: from the stored address, and — with the broadcast cut off,
//     as a NAT router does — by sweeping the subnet when the stored host is a name that does not resolve
//   - the firmware search with the wrong key finds nothing (and so would never store the address)
//   - DISCOVERY_PORT=0 switches the responder off; a taken port does not stop the relay
//
// Own ports, throwaway log files in a temp folder. Core modules + g++ (the
// firmware-search part is skipped when g++ is missing). Takes about 15 seconds.
//
// Usage:  node tests/e2e/test_discovery_e2e.js
//         E2E_DISC_HTTP=19340 E2E_DISC_TCP=19341 E2E_DISC_UDP=19342 node tests/e2e/test_discovery_e2e.js
// Exit code: 0 if every check passed, 1 otherwise.

const dgram = require('dgram');
const fs = require('fs');
const net = require('net');
const os = require('os');
const path = require('path');
const { spawn, spawnSync } = require('child_process');

// __dirname = nodejs/esp-relay/tests/e2e → ROOT = nodejs/esp-relay
const ROOT = path.join(__dirname, '..', '..');
const TOOL_SRC = path.join(__dirname, '..', '..', '..', '..', 'tests', 'cpp', 'relay_find_tool.cpp');
const discovery = require(path.join(ROOT, 'lib/discovery'));
const HTTP_PORT = Number(process.env.E2E_DISC_HTTP || 19340);
const TCP_PORT = Number(process.env.E2E_DISC_TCP || 19341);
const UDP_PORT = Number(process.env.E2E_DISC_UDP || 19342);
const RELAY_KEY = 'e2e-discovery-relay-key';
const NODE_KEY = 'e2e-discovery-node-key';

let passCount = 0;
let failCount = 0;
let finished = false;
process.on('exit', () => {
  if (!finished) { console.error('[FAIL] the test ended without finishing (no summary was reached)'); process.exitCode = 1; }
});
function check(name, cond, detail) {
  if (cond) { passCount += 1; console.log(`[PASS] ${name}`); }
  else { failCount += 1; console.error(`[FAIL] ${name}${detail ? ' -- ' + detail : ''}`); }
}
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

let server = null;
let tmpDir = null;

function startRelay(env = {}, wantLine = null) {
  tmpDir = fs.mkdtempSync(path.join(os.tmpdir(), 'espwifinet-disc-e2e-'));
  server = spawn(process.execPath, [path.join(ROOT, 'server.js')], {
    cwd: ROOT,
    env: {
      ...process.env,
      PORT: String(HTTP_PORT),
      NODE_PORT: String(TCP_PORT),
      RELAY_KEY,
      NODE_KEY,
      DATA_DIR: path.join(tmpDir, 'data'),
      LOG_DIR: path.join(tmpDir, 'logs'),
      FIRMWARE_DIR: path.join(tmpDir, 'firmware'),
      ...env,
    },
    stdio: ['ignore', 'pipe', 'pipe'],
  });
  let out = '';
  server.stdout.on('data', (d) => { out += d.toString(); });
  server.stderr.on('data', (d) => { out += d.toString(); });
  const udpPort = Number(env.DISCOVERY_PORT || process.env.E2E_DISC_UDP || UDP_PORT);
  const wanted = wantLine || (env.DISCOVERY_PORT === '0'
    ? '[relay] discovery is off (discoveryPort = 0)'
    : `[relay] discovery listening on UDP :${udpPort}`);
  return new Promise((resolve, reject) => {
    const poll = setInterval(() => {
      if (out.includes(wanted)) {
        clearInterval(poll);
        resolve({ output: () => out });
      }
    }, 100);
    server.on('exit', (code) => {
      clearInterval(poll);
      reject(new Error(`relay exited early (code ${code}): ${out}`));
    });
    setTimeout(() => {
      clearInterval(poll);
      reject(new Error(`relay not ready in 8s (wanted "${wanted}"): ${out}`));
    }, 8000).unref();
  });
}

function stopRelay() {
  return new Promise((resolve) => {
    const child = server;
    server = null;
    if (!child || child.exitCode !== null || child.signalCode !== null) return resolve();
    child.once('exit', () => resolve());
    child.kill('SIGTERM');
    setTimeout(() => { try { child.kill('SIGKILL'); } catch (e) { /* dead */ } resolve(); }, 2000);
  });
}

function relayLog() {
  const file = path.join(tmpDir, 'logs', 'relay.log');
  return fs.existsSync(file) ? fs.readFileSync(file, 'utf8') : '';
}

// Sends one datagram to the relay and returns the answer (or null if none comes within waitMs).
function ask(payload, waitMs = 400, port = UDP_PORT) {
  return new Promise((resolve) => {
    const sock = dgram.createSocket('udp4');
    let done = false;
    const finish = (v) => { if (!done) { done = true; try { sock.close(); } catch (e) { /* closed */ } resolve(v); } };
    sock.on('message', (m) => finish(m));
    sock.on('error', () => finish(null));
    sock.send(payload, port, '127.0.0.1', (err) => { if (err) finish(null); });
    setTimeout(() => finish(null), waitMs);
  });
}

function runTool(bin, args) {
  const r = spawnSync(bin, args, { encoding: 'utf8', timeout: 60000 });
  return { status: r.status, out: (r.stdout || '').trim(), err: (r.stderr || '').trim() };
}

async function main() {
  let blocker = null;
  try {
    // The firmware's search, compiled as C++11 like the board's toolchain.
    let bin = null;
    if (process.env.E2E_DISC_SKIP_TOOL !== '1') {
      bin = path.join(os.tmpdir(), 'espnet_relay_find_tool');
      const cc = spawnSync('g++', ['-std=c++11', '-Wall', '-Wextra', '-o', bin, TOOL_SRC], { encoding: 'utf8' });
      check('the firmware search compiles as C++11 for the desktop', cc.status === 0, cc.stderr);
      if (cc.status !== 0) bin = null;
    }

    await startRelay({ DISCOVERY_PORT: String(UDP_PORT) });

    // ── the relay's side, with raw datagrams ──
    const nonce = '00112233445566ff';
    const reply = await ask(Buffer.from('ESPNET1?' + nonce));
    check('a valid probe is answered', reply !== null);
    check('the answer is the exact bytes the protocol defines', reply !== null && reply.equals(discovery.buildReply(nonce, NODE_KEY)), reply && reply.toString('latin1'));
    check('the answer verifies with the node key, and only with it', reply !== null && discovery.verifyReply(reply, nonce, NODE_KEY) && !discovery.verifyReply(reply, nonce, 'some-other-key'));
    check('the node key itself is not in the answer', reply !== null && !reply.toString('latin1').includes(NODE_KEY));

    const silent = [
      ['junk text', Buffer.from('hello')],
      ['an empty datagram', Buffer.alloc(0)],
      ['a probe with an upper-case nonce', Buffer.from('ESPNET1?00112233445566FF')],
      ['a probe with a short nonce', Buffer.from('ESPNET1?0011')],
      ['a probe with something after the nonce', Buffer.from('ESPNET1?00112233445566ff\n')],
      ['an answer sent as if it were a probe', Buffer.from('ESPNET1!00112233445566ff')],
      ['a big datagram', Buffer.alloc(1400, 0x41)],
    ];
    for (const [what, buf] of silent) {
      await sleep(120); // the relay limits answers per address; stay under it so only the datagram decides
      check(`${what}: no answer`, (await ask(buf, 250)) === null);
    }
    await sleep(120);
    check('the relay still answers after all that junk', (await ask(Buffer.from('ESPNET1?' + nonce))) !== null);

    check('the relay log says a node asked where the relay is', /asked where the relay is/.test(relayLog()), relayLog().slice(-400));

    // ── the firmware's real search against the real relay ──
    if (bin) {
      let r = runTool(bin, ['127.0.0.9', NODE_KEY, String(UDP_PORT), '127.0.0.250']);
      check('the firmware search finds the relay from a stored address', r.status === 0 && /^FOUND ip=127\./.test(r.out), JSON.stringify(r));

      r = runTool(bin, ['raspberrypi.local', NODE_KEY, String(UDP_PORT), '127.0.0.250']);
      check('with a stored name that does not resolve, the relay is still found', r.status === 0 && /^FOUND ip=/.test(r.out), JSON.stringify(r));

      // Behind a NAT router a broadcast goes nowhere: only the sweep of the subnet can find the relay.
      r = runTool(bin, ['raspberrypi.local', NODE_KEY, String(UDP_PORT), '127.0.0.250', 'nat']);
      check('behind a NAT router (no broadcast), the sweep of the node\'s subnet finds the relay', r.status === 0 && /^FOUND ip=127\./.test(r.out), JSON.stringify(r));
      r = runTool(bin, ['127.0.0.9', 'not-the-key', String(UDP_PORT), '127.0.0.250', 'nat']);
      check('...and with the wrong key even the sweep finds nothing', r.status === 3, JSON.stringify(r));

      r = runTool(bin, ['127.0.0.9', 'not-the-key', String(UDP_PORT), '127.0.0.250']);
      check('the firmware search with the wrong key finds nothing', r.status === 3 && r.out === 'NOT_FOUND', JSON.stringify(r));
    }

    // ── off, and taken ──
    await stopRelay();
    const offRelay = await startRelay({ DISCOVERY_PORT: '0' });
    check('DISCOVERY_PORT=0 switches the responder off (and says so)', /discovery is off \(discoveryPort = 0\)/.test(offRelay.output()), offRelay.output());
    check('...and nothing answers', (await ask(Buffer.from('ESPNET1?' + nonce), 400)) === null);
    await stopRelay();

    blocker = dgram.createSocket('udp4');
    await new Promise((res) => blocker.bind(UDP_PORT, '0.0.0.0', res));
    // the discovery socket cannot bind: the relay says so and carries on —
    // readiness here is the HTTP line, not the "listening on UDP" line
    await startRelay({ DISCOVERY_PORT: String(UDP_PORT) }, '[relay] http api on :');
    await sleep(500);
    const httpUp = await new Promise((res) => {
      const s = net.connect(HTTP_PORT, '127.0.0.1', () => { s.destroy(); res(true); });
      s.on('error', () => res(false));
    });
    check('a taken UDP port does not stop the relay (HTTP still up)', httpUp && server && server.exitCode === null);
    check('...and the relay says discovery stopped', /discovery responder stopped/.test(relayLog()), relayLog().slice(-400));
  } catch (err) {
    check('the test ran to the end', false, err && err.stack);
  } finally {
    if (blocker) try { blocker.close(); } catch (e) { /* closed */ }
    await stopRelay();
    try { fs.rmSync(tmpDir, { recursive: true, force: true }); } catch (e) { /* busy */ }
  }
  finished = true;
  console.log(`\n${passCount} passed, ${failCount} failed`);
  process.exitCode = failCount === 0 ? 0 : 1;
}

main();
