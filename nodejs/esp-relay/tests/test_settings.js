'use strict';

// test_settings.js — validation rules for the settings page.

const assert = require('assert');
const { validateSettings, loadSettings, SettingsStore, DEFAULT_SETTINGS } = require('../lib/settings');
const { loadGpioState, saveGpioState, isGpioStateValid } = require('../lib/gpiostate');

const readJson = (file, fallback) => fallback;

function run() {
  // defaults
  const empty = validateSettings({});
  assert.strictEqual(empty.ok, true);
  assert.deepStrictEqual(empty.value, DEFAULT_SETTINGS);
  assert.strictEqual(empty.value.gpioPin, 5);

  // valid pin
  const p5 = validateSettings({ gpioPin: 5, names: { 'node-1': 'Boiler pump' } });
  assert.strictEqual(p5.ok, true);
  assert.strictEqual(p5.value.names['node-1'], 'Boiler pump');
  assert.strictEqual(p5.warnings.length, 0);

  // risky pins warn but pass
  const p2 = validateSettings({ gpioPin: 2 });
  assert.strictEqual(p2.ok, true);
  assert.ok(p2.warnings[0].includes('strap'));
  const p16 = validateSettings({ gpioPin: 16 });
  assert.strictEqual(p16.ok, true);
  assert.ok(p16.warnings[0].includes('special'));

  // out of range / wrong type (numeric strings are accepted — they unambiguously convert)
  assert.strictEqual(validateSettings({ gpioPin: '5' }).ok, true);
  for (const pin of [-1, 17, 4.5, 'five', null]) {
    const r = validateSettings({ gpioPin: pin });
    assert.strictEqual(r.ok, false, `pin ${JSON.stringify(pin)} must be refused`);
  }

  // statusIntervalMs bounds
  assert.strictEqual(validateSettings({ statusIntervalMs: 2000 }).ok, true);
  assert.strictEqual(validateSettings({ statusIntervalMs: 600000 }).ok, true);
  assert.strictEqual(validateSettings({ statusIntervalMs: 1000 }).ok, false);
  assert.strictEqual(validateSettings({ statusIntervalMs: 600001 }).ok, false);

  // offlineAfterMs (#36): bounds and the 3x-statusInterval rule
  assert.strictEqual(validateSettings({ offlineAfterMs: 45000 }).ok, true);
  assert.strictEqual(validateSettings({ offlineAfterMs: 30000 }).ok, true); // exactly 3x the default interval
  assert.strictEqual(validateSettings({ offlineAfterMs: '45000' }).ok, true); // numeric strings convert
  assert.strictEqual(validateSettings({ offlineAfterMs: 29999 }).ok, false);
  assert.strictEqual(validateSettings({ offlineAfterMs: 3600001 }).ok, false);
  assert.strictEqual(validateSettings({ offlineAfterMs: 4.5 }).ok, false);
  const w3x = validateSettings({ statusIntervalMs: 60000, offlineAfterMs: 60000 });
  assert.strictEqual(w3x.ok, false);
  assert.ok(w3x.errors[0].includes('3x statusIntervalMs'));

  // minLogLevel (#36): the three queue levels, nothing else
  assert.strictEqual(validateSettings({ minLogLevel: 'info' }).ok, true);
  assert.strictEqual(validateSettings({ minLogLevel: 'warn' }).ok, true);
  assert.strictEqual(validateSettings({ minLogLevel: 'error' }).ok, true);
  assert.strictEqual(validateSettings({ minLogLevel: 'DEBUG' }).ok, false);
  assert.strictEqual(validateSettings({ minLogLevel: 1 }).ok, false);

  // setupMode (#135 v2): the SETUP_MODE maintenance switch. Strict boolean —
  // the string "false" must NOT arm the fleet, absent = off, default off.
  assert.strictEqual(validateSettings({}).value.setupMode, false);
  assert.strictEqual(validateSettings({ setupMode: true }).ok, true);
  assert.strictEqual(validateSettings({ setupMode: true }).value.setupMode, true);
  assert.strictEqual(validateSettings({ setupMode: false }).ok, true);
  assert.strictEqual(validateSettings({ setupMode: false }).value.setupMode, false);
  assert.strictEqual(validateSettings({ setupMode: 'false' }).ok, false);
  assert.strictEqual(validateSettings({ setupMode: 'true' }).ok, false);
  assert.strictEqual(validateSettings({ setupMode: 1 }).ok, false);
  assert.strictEqual(validateSettings({ minLogLevel: null }).ok, false);

  // SettingsStore.existed: config values stay in charge until the first save
  const savedStore = new SettingsStore('/unused', () => ({ gpioPin: 6, offlineAfterMs: 90000, minLogLevel: 'error' }));
  assert.strictEqual(savedStore.existed, true);
  assert.strictEqual(savedStore.get().offlineAfterMs, 90000);
  const freshStore = new SettingsStore('/unused', () => null);
  assert.strictEqual(freshStore.existed, false);
  assert.strictEqual(freshStore.get().minLogLevel, 'info');

  // names: bad ids / bad names refused, unknown keys dropped
  const n1 = validateSettings({ names: { 'bad id!': 'x' } });
  assert.strictEqual(n1.ok, false);
  const n2 = validateSettings({ names: { 'node-1': '' } });
  assert.strictEqual(n2.ok, false);
  const n3 = validateSettings({ names: { 'node-1': 'x'.repeat(41) } });
  assert.strictEqual(n3.ok, false);
  const n4 = validateSettings({ gpioPin: 4, nonsense: 'drop me' });
  assert.strictEqual(n4.ok, true);
  assert.strictEqual(n4.value.nonsense, undefined);

  // loadSettings: corrupt file falls back to defaults with a warning
  const brokenRead = () => { throw new Error('boom'); };
  const loaded = loadSettings('/unused', brokenRead);
  assert.deepStrictEqual(loaded.settings, DEFAULT_SETTINGS);
  assert.strictEqual(loaded.warnings.length, 1);

  // ── gpio state ──
  assert.strictEqual(isGpioStateValid('on'), true);
  assert.strictEqual(isGpioStateValid('off'), true);
  assert.strictEqual(isGpioStateValid('ON'), false);
  assert.strictEqual(isGpioStateValid('1'), false);
  assert.strictEqual(loadGpioState('/unused', readJson), 'off'); // default
  assert.strictEqual(loadGpioState('/unused', () => ({ gpioState: 'on' })), 'on');
  assert.strictEqual(loadGpioState('/unused', () => ({ gpioState: 'bogus' })), 'off');
  const saved = saveGpioState('/unused', 'on', readJson, () => {});
  assert.strictEqual(saved.ok, true);
  const bad = saveGpioState('/unused', 'maybe', readJson, () => {});
  assert.strictEqual(bad.ok, false);
  const unwritable = saveGpioState('/unused', 'on', readJson, () => { throw new Error('disk full'); });
  assert.strictEqual(unwritable.ok, false);
  assert.ok(unwritable.errors[0].includes('disk full'));
}

module.exports = { run };
