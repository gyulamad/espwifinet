'use strict';

// test_devices.js — device record handling and the snapshot the dashboard sees.

const assert = require('assert');
const d = require('../lib/devices');

function run() {
  // connection classification rules
  assert.strictEqual(d.isConnectionConflict(500), true); // still talking: two senders
  assert.strictEqual(d.isConnectionConflict(5000), false); // silent: a reconnect

  // hello + status application
  const now = 1000;
  const dev = d.createDeviceInfo('node-1', now);
  assert.strictEqual(dev.online, true);
  d.applyHelloMessage(dev, {
    type: 'hello', fw: '1.0.0', chip: 'esp8266', ssid: 'home', rssi: -60,
    heapFree: 20480, uptimeMs: 1234, gpioPin: 5, gpioState: 'off',
  });
  assert.strictEqual(dev.fwVersion, '1.0.0');
  assert.strictEqual(dev.chip, 'esp8266');
  assert.strictEqual(dev.ssid, 'home');
  assert.strictEqual(dev.rssi, -60);
  assert.strictEqual(dev.gpioPin, 5);

  // status update overwrites fields it carries, keeps the rest
  d.applyStatusMessage(dev, { type: 'status', rssi: -70, uptimeMs: 5000 });
  assert.strictEqual(dev.rssi, -70);
  assert.strictEqual(dev.ssid, 'home');
  assert.strictEqual(dev.fwVersion, '1.0.0');

  // hostile/nonsense fields are ignored, not stored
  d.applyStatusMessage(dev, { type: 'status', rssi: 'drop', ssid: 'x'.repeat(99), gpioState: 'maybe', gpioPin: 99 });
  assert.strictEqual(dev.rssi, -70);
  assert.strictEqual(dev.ssid, 'home');
  assert.strictEqual(dev.gpioState, 'off');
  assert.strictEqual(dev.gpioPin, 5);

  // rssi bars
  assert.strictEqual(d.rssiToBars(-50), 4);
  assert.strictEqual(d.rssiToBars(-55), 4);
  assert.strictEqual(d.rssiToBars(-60), 3);
  assert.strictEqual(d.rssiToBars(-66), 3);
  assert.strictEqual(d.rssiToBars(-75), 2);
  assert.strictEqual(d.rssiToBars(-85), 1);
  assert.strictEqual(d.rssiToBars(-95), 0);
  assert.strictEqual(d.rssiToBars(null), 0);

  // snapshot: online vs offline, sorted by id, friendly names
  const fleet = new Map();
  const b = d.createDeviceInfo('b-node', now);
  b.ssid = 'net';
  b.rssi = -50;
  b.lastSeen = now - 1000; // fresh
  const a = d.createDeviceInfo('a-node', now);
  a.lastSeen = now - 60000; // stale
  fleet.set('b-node', b);
  fleet.set('a-node', a);
  const snap = d.buildSnapshot(fleet, (id) => (id === 'a-node' ? 'Alpha' : id), now + 1000, 45000);
  assert.strictEqual(snap.length, 2);
  assert.strictEqual(snap[0].deviceId, 'a-node'); // sorted
  assert.strictEqual(snap[0].name, 'Alpha');
  assert.strictEqual(snap[0].online, false);
  assert.strictEqual(snap[0].lastSeenAgoMs, 61000);
  assert.strictEqual(snap[1].online, true);
  assert.strictEqual(snap[1].signalBars, 4);
  assert.strictEqual(snap[1].lastSeenAgoMs, 0);
}

module.exports = { run };
