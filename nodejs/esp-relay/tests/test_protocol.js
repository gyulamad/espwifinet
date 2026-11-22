'use strict';

// test_protocol.js — framing and auth-line rules (pure, hand-built buffers).

const assert = require('assert');
const p = require('../lib/protocol');

function run() {
  // ── auth line ──
  const line = Buffer.from('node-1\tsecret\n');
  assert.strictEqual(p.findAuthLineEnd(line), 13);
  assert.strictEqual(p.findAuthLineEnd(Buffer.from('partial')), -1);
  assert.deepStrictEqual(p.parseAuthLine(Buffer.from('node-1\tsecret')), { deviceId: 'node-1', apiKey: 'secret' });
  assert.deepStrictEqual(p.parseAuthLine(Buffer.from('node-1\tsecret\r')), { deviceId: 'node-1', apiKey: 'secret' });
  assert.strictEqual(p.parseAuthLine(Buffer.from('no-tab-here')), null);
  assert.strictEqual(p.parseAuthLine(Buffer.from('\tsecret')), null);
  assert.strictEqual(p.parseAuthLine(Buffer.from('node-1\t')), null);
  assert.strictEqual(p.parseAuthLine(Buffer.from('node\x01\tsecret')), null); // control byte in id
  assert.strictEqual(p.isAuthLineTooLong(257), true);
  assert.strictEqual(p.isAuthLineTooLong(200), false);

  // ── device id rules ──
  assert.strictEqual(p.isDeviceIdValid('node-1'), true);
  assert.strictEqual(p.isDeviceIdValid('ESP_123456.ab-c'), true);
  assert.strictEqual(p.isDeviceIdValid(''), false);
  assert.strictEqual(p.isDeviceIdValid('../etc'), false); // slash -> path escape refused
  assert.strictEqual(p.isDeviceIdValid('sp ace'), false);
  assert.strictEqual(p.isDeviceIdValid('x'.repeat(65)), false);

  // ── message framing ──
  assert.strictEqual(p.isMessageLengthValid(0), false);
  assert.strictEqual(p.isMessageLengthValid(10), true);
  assert.strictEqual(p.isMessageLengthValid(p.MAX_MESSAGE_BYTES + 1), false);

  const encoded = p.encodeMessage({ type: 'status', rssi: -60 });
  assert.ok(encoded);
  assert.strictEqual(encoded.readUInt32BE(0), encoded.length - 4);
  const drained = p.drainMessages(Buffer.concat([encoded, Buffer.from([0, 0, 0])]));
  assert.strictEqual(drained.messages.length, 1);
  assert.strictEqual(JSON.parse(drained.messages[0].toString('utf8')).rssi, -60);
  assert.deepStrictEqual(Array.from(drained.rest), [0, 0, 0]); // partial prefix kept

  // two messages in one chunk
  const two = Buffer.concat([encoded, encoded]);
  const d2 = p.drainMessages(two);
  assert.strictEqual(d2.messages.length, 2);
  assert.strictEqual(d2.rest.length, 0);

  // invalid length prefix throws with a typed error
  const bad = Buffer.alloc(4);
  bad.writeUInt32BE(p.MAX_MESSAGE_BYTES + 1, 0);
  assert.throws(() => p.drainMessages(bad), (e) => e.code === 'INVALID_MESSAGE_LENGTH');

  // ── encode guards ──
  assert.strictEqual(p.encodeMessage(null), null);
  assert.strictEqual(p.encodeMessage('nope'), null);
  const circular = {};
  circular.self = circular;
  assert.strictEqual(p.encodeMessage(circular), null);
  const big = { type: 'log', msg: 'x'.repeat(p.MAX_MESSAGE_BYTES) };
  assert.strictEqual(p.encodeMessage(big), null);

  // ── classify ──
  for (const t of p.NODE_MESSAGE_TYPES) {
    const m = p.classifyMessage(Buffer.from(JSON.stringify({ type: t })));
    assert.strictEqual(m.type, t);
  }
  assert.strictEqual(p.classifyMessage(Buffer.from('not json')), null);
  assert.strictEqual(p.classifyMessage(Buffer.from('[1,2]')), null);
  assert.strictEqual(p.classifyMessage(Buffer.from(JSON.stringify({ type: 'unknown' }))), null);
  assert.strictEqual(p.classifyMessage(Buffer.from(JSON.stringify({ noType: 1 }))), null);
  assert.strictEqual(p.classifyMessage(Buffer.alloc(0)), null);
}

module.exports = { run };
