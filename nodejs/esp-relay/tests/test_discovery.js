'use strict';

// test_discovery.js — the relay's half of "a node finds the relay by itself"
// (task #160, lib/discovery.js + lib/throttle.js): which datagrams are
// probes, what the answer looks like, and that only the holder of nodeKey
// can make a correct one. The firmware's half is tested in
// tests/cpp/test_relay_find.cpp against the same fixed vector, so the two
// cannot drift apart unnoticed.

const crypto = require('crypto');
const assert = require('assert');
const d = require('../lib/discovery');
const { Throttle } = require('../lib/throttle');

const NONCE = '0123456789abcdef';
const KEY = 'change-me';
// HMAC-SHA256(key "change-me", "ESPNET1!0123456789abcdef"), worked out independently of Node with
//   printf 'ESPNET1!0123456789abcdef' | openssl dgst -sha256 -hmac 'change-me'
// tests/cpp/test_relay_find.cpp holds the same constant.
const VECTOR_MAC = '0259fef1f2b658a2504d643ffcc048ee8bddccd5578ae388a7baafaa9463f105';

function run() {
  // ── parseProbe ──
  assert.strictEqual(d.parseProbe(Buffer.from('ESPNET1?' + NONCE)), NONCE);
  assert.strictEqual(d.parseProbe(Buffer.from('ESPNET1?ffffffffffffffff')), 'ffffffffffffffff');

  const bad = [
    '', 'ESPNET1?', 'ESPNET1?' + NONCE + '0', 'ESPNET1?' + NONCE.slice(0, 15),
    'ESPNET1?' + NONCE.toUpperCase(), 'ESPNET1?' + 'g'.repeat(16), 'espnet1?' + NONCE,
    'ESPNET2?' + NONCE, 'ESPNET1!' + NONCE, ' ESPNET1?' + NONCE, 'ESPNET1?' + NONCE + '\n',
    'GET / HTTP/1.1\r\n\r\n', 'x'.repeat(24), 'x'.repeat(2000),
  ];
  for (const s of bad) {
    assert.strictEqual(d.parseProbe(Buffer.from(s, 'latin1')), null, JSON.stringify(s.slice(0, 30)));
  }
  assert.strictEqual(d.parseProbe('ESPNET1?' + NONCE), null, 'a string is not a datagram');
  assert.strictEqual(d.parseProbe(null), null);
  assert.strictEqual(d.parseProbe(undefined), null);

  // ── buildReply ──
  const reply = d.buildReply(NONCE, KEY).toString('latin1');
  assert.ok(/^ESPNET1!0123456789abcdef:[0-9a-f]{64}$/.test(reply), reply);
  assert.strictEqual(reply.length, 8 + 16 + 1 + 64);
  const expected = crypto.createHmac('sha256', KEY).update('ESPNET1!' + NONCE).digest('hex');
  assert.strictEqual(d.macFor(KEY, NONCE), expected);
  assert.strictEqual(d.buildReply(NONCE, KEY).toString('latin1'), `ESPNET1!${NONCE}:${expected}`);
  assert.strictEqual(d.macFor(KEY, NONCE), VECTOR_MAC, 'the fixed vector the firmware tests also use');

  const base = d.macFor(KEY, NONCE);
  assert.notStrictEqual(d.macFor('other-key', NONCE), base, 'a different key gives a different MAC');
  assert.notStrictEqual(d.macFor(KEY, 'fedcba9876543210'), base, 'a different nonce gives a different MAC');

  assert.strictEqual(d.buildReply(NONCE, ''), null, 'an unset nodeKey must not be a key anyone can use');
  assert.strictEqual(d.buildReply(NONCE, undefined), null);
  assert.strictEqual(d.buildReply(NONCE, 123), null);
  assert.strictEqual(d.buildReply('short', KEY), null);
  assert.strictEqual(d.buildReply(NONCE.toUpperCase(), KEY), null);

  // ── verifyReply ──
  const good = d.buildReply(NONCE, KEY);
  assert.ok(d.verifyReply(good, NONCE, KEY), 'the real answer');
  assert.ok(!d.verifyReply(good, NONCE, 'other-key'), 'someone who does not know the key cannot be checked by it');
  assert.ok(!d.verifyReply(d.buildReply(NONCE, 'other-key'), NONCE, KEY), 'an answer made with another key');
  assert.ok(!d.verifyReply(good, 'fedcba9876543210', KEY), 'an answer to a different probe (replay)');
  assert.ok(!d.verifyReply(Buffer.concat([good, Buffer.from('0')]), NONCE, KEY), 'longer');
  assert.ok(!d.verifyReply(good.subarray(0, good.length - 1), NONCE, KEY), 'shorter');
  const flipped = Buffer.from(good);
  flipped[flipped.length - 1] ^= 1;
  assert.ok(!d.verifyReply(flipped, NONCE, KEY), 'one changed bit');
  assert.ok(!d.verifyReply(good, NONCE, ''), 'empty key');
  assert.ok(!d.verifyReply('not a buffer', NONCE, KEY), 'not a buffer');

  // ── no amplification: the answer stays small ──
  const probe = Buffer.from('ESPNET1?' + NONCE);
  assert.ok(good.length <= probe.length * 4, `${good.length} bytes for a ${probe.length}-byte probe`);

  // ── resolveDiscoveryPort ──
  assert.deepStrictEqual(d.resolveDiscoveryPort(undefined), { port: 8083, warnings: [] });
  assert.deepStrictEqual(d.resolveDiscoveryPort(null), { port: 8083, warnings: [] });
  assert.deepStrictEqual(d.resolveDiscoveryPort(''), { port: 8083, warnings: [] });
  assert.deepStrictEqual(d.resolveDiscoveryPort(9000), { port: 9000, warnings: [] });
  assert.deepStrictEqual(d.resolveDiscoveryPort('9001'), { port: 9001, warnings: [] }, 'an environment variable is text');
  assert.deepStrictEqual(d.resolveDiscoveryPort(0), { port: 0, warnings: [] });
  assert.deepStrictEqual(d.resolveDiscoveryPort('0'), { port: 0, warnings: [] });
  for (const badPort of [-1, 65536, 80.5, 'abc', '80x', '-5', '1e3', NaN, true, false, {}, []]) {
    const r = d.resolveDiscoveryPort(badPort);
    assert.strictEqual(r.port, 8083, String(badPort));
    assert.strictEqual(r.warnings.length, 1, String(badPort));
  }

  // ── Throttle (the answer-gap / log-gap limiter server.js uses) ──
  const t = new Throttle(1000);
  assert.deepStrictEqual(t.check('a', 0), { allow: true, suppressed: 0 });
  assert.deepStrictEqual(t.check('a', 10), { allow: false, suppressed: 0 });
  assert.deepStrictEqual(t.check('a', 20), { allow: false, suppressed: 0 });
  assert.deepStrictEqual(t.check('a', 1000), { allow: true, suppressed: 2 }, 'the window passed: allowed, with the skip count');
  assert.deepStrictEqual(t.check('a', 1001), { allow: false, suppressed: 0 });
  assert.deepStrictEqual(t.check('b', 20), { allow: true, suppressed: 0 }, 'another address is independent');
  // a clock going backwards must not lock the key out forever
  assert.deepStrictEqual(t.check('c', 5000), { allow: true, suppressed: 0 });
  assert.deepStrictEqual(t.check('c', 100), { allow: true, suppressed: 0 });
  // keys that keep changing are forgotten once their window passes (bounded memory)
  const big = new Throttle(1000, 10);
  for (let i = 0; i < 50; ++i) big.check(`key-${i}`, 100 + i);
  assert.strictEqual(big._state.size, 50, 'live keys stay (they are all inside one window)');
  big.check('key-new', 5000); // a fresh key: the expired ones are forgotten now
  assert.strictEqual(big._state.has('key-0'), false, 'expired keys pruned');
  assert.strictEqual(big._state.has('key-new'), true, 'the live key stays');
}

module.exports = { run };
