'use strict';

// test_logwriter.js — JSON-lines format, rotation, tail, damaged-line skip.

const assert = require('assert');
const fs = require('fs');
const os = require('os');
const path = require('path');
const { formatLogLine, rotateIfNeeded, tailLog, LogWriter, logFilePath } = require('../lib/logwriter');

function run() {
  // line format: one JSON object per line with ts/level/device/msg
  const line = formatLogLine({ level: 'warn', device: 'node-1', msg: 'wifi lost' }, '2026-11-22T10:00:00.000Z');
  const obj = JSON.parse(line);
  assert.strictEqual(obj.level, 'warn');
  assert.strictEqual(obj.device, 'node-1');
  assert.strictEqual(obj.msg, 'wifi lost');
  assert.strictEqual(obj.ts, '2026-11-22T10:00:00.000Z');
  assert.ok(line.endsWith('\n'));

  // path builder keeps the id inside the log dir
  assert.strictEqual(path.basename(logFilePath('/logs', 'node-1')), 'node-1.log');

  // real IO in a temp dir
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'espwifinet-test-'));
  const writer = new LogWriter(dir);

  writer.append({ device: 'node-1', level: 'info', msg: 'first' }, '2026-11-22T10:00:01.000Z');
  writer.append({ device: 'node-1', level: 'error', msg: 'second' }, '2026-11-22T10:00:02.000Z');
  writer.append({ device: 'relay', level: 'info', msg: 'relay line' }, '2026-11-22T10:00:03.000Z');

  const t = writer.tail('node-1', 10);
  assert.strictEqual(t.length, 2);
  assert.strictEqual(t[0].msg, 'second'); // newest first
  assert.strictEqual(t[1].msg, 'first');
  assert.deepStrictEqual(writer.listDevices(), ['node-1', 'relay']);

  // tail with fewer lines than present
  assert.strictEqual(writer.tail('node-1', 1).length, 1);

  // missing device file -> empty, no throw
  assert.deepStrictEqual(writer.tail('gone', 5), []);

  // damaged line is skipped, neighbours survive
  const file = logFilePath(dir, 'node-1');
  fs.appendFileSync(file, '{not json}\n');
  const t2 = writer.tail('node-1', 10);
  assert.strictEqual(t2.length, 2); // damaged line invisible

  // rotation: file over the cap moves to .old, appending continues
  const small = new LogWriter(dir, 10);
  small.append({ device: 'rot', msg: 'a' }, '2026-11-22T10:00:01.000Z');
  small.append({ device: 'rot', msg: 'b' }, '2026-11-22T10:00:02.000Z'); // > 10 bytes -> rotated
  assert.strictEqual(fs.existsSync(`${logFilePath(dir, 'rot')}.old`), true);
  assert.strictEqual(tailLog(logFilePath(dir, 'rot'), 10).length, 1);
  assert.strictEqual(tailLog(logFilePath(dir, 'rot'), 10)[0].msg, 'b');

  // rotateIfNeeded on a missing file is a no-op
  assert.strictEqual(rotateIfNeeded(path.join(dir, 'missing.log')), false);

  fs.rmSync(dir, { recursive: true, force: true });
}

module.exports = { run };
