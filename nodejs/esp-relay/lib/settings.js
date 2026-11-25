'use strict';

// settings.js — what the operator configures from the dashboard's settings
// page, and the rules for accepting it. The validation lives here as pure
// functions (unit tested); the file IO lives in SettingsStore (thin).
//
// Settings shape (#36: every previously hard-coded value is on the page):
//   {
//     gpioPin: 5,                      // the GPIO leg every node switches (task: "one toggle ... on each esp")
//     gpioActiveHigh: true,            // true: on = HIGH, off = LOW; false: inverted (some relay boards are active-low)
//     statusIntervalMs: 10000,         // how often nodes report status
//     offlineAfterMs: 45000,           // silence before a node counts as offline (>= 3x the status interval)
//     minLogLevel: 'info',             // lowest level nodes queue for the relay (info | warn | error)
//     names: { "<deviceId>": "Boiler" }   // friendly names, per device
//   }
//
// The GPIO pin is one global value on purpose: the dashboard has exactly one
// toggle, and every node must drive the same leg so the toggle means the same
// thing everywhere. Per-device overrides can come later as a subtask.

const { writeFileAtomic } = require('./util');

const DEFAULT_SETTINGS = {
  gpioPin: 5,
  gpioActiveHigh: true,
  statusIntervalMs: 10000,
  offlineAfterMs: 45000,
  minLogLevel: 'info',
  names: {},
};

// Pins that strap boot modes on the ESP8266 or belong to flash/serial. They
// WORK in some configurations but a node can then fail to boot with the pin
// pulled low/high. Not forbidden (the operator may know better) — validated
// as a warning the dashboard shows next to the save button.
const RISKY_GPIO_PINS = [0, 1, 2, 3, 15];

// The widest valid range across the boards the fleet runs (#131): the
// ESP8266 stops at 16, the ESP32 at 33. The setting is one global value on
// purpose (one toggle, one leg), so a mixed fleet shares it — each NODE
// decides whether ITS board can actually drive the pushed pin (board.h's
// gpioPinAllowed: an ESP8266 keeps its leg when 17+ arrives, an ESP32
// refuses its flash pins 6..11) and keeps the previous one otherwise. The
// node's gpio_ack then tells the dashboard what really happened.
function isGpioPinAllowed(pin) {
  return Number.isInteger(pin) && pin >= 0 && pin <= 33;
}

// Validates one settings object. Returns { ok: true, value, warnings } or
// { ok: false, errors }. Unknown keys are dropped, so the stored file only
// ever contains keys this relay understands (forward-compatible saves).
function validateSettings(input) {
  const errors = [];
  const warnings = [];
  const value = { ...DEFAULT_SETTINGS, names: {} };

  if (input === null || typeof input !== 'object' || Array.isArray(input)) {
    return { ok: false, errors: ['settings must be an object'] };
  }

  if (input.gpioPin === undefined) {
    value.gpioPin = DEFAULT_SETTINGS.gpioPin;
  } else if (typeof input.gpioPin === 'number' || (typeof input.gpioPin === 'string' && input.gpioPin.trim() !== '')) {
    // Number(null)/Number('') are 0 — a valid but boot-strap pin — so null and
    // blank are refused above instead of silently meaning pin 0.
    const pin = Number(input.gpioPin);
    if (!Number.isInteger(pin) || pin < 0 || pin > 16) {
      errors.push('gpioPin must be an integer between 0 and 16');
    } else {
      value.gpioPin = pin;
      if (RISKY_GPIO_PINS.includes(pin)) warnings.push(`GPIO ${pin} is a boot/serial strap pin — the node may fail to boot with it wired`);
      if (pin === 16) warnings.push('GPIO 16 is a special (wake) pin — avoid it for relays');
    }
  } else {
    errors.push('gpioPin must be an integer between 0 and 16');
  }

  value.gpioActiveHigh = input.gpioActiveHigh === undefined
    ? DEFAULT_SETTINGS.gpioActiveHigh
    : Boolean(input.gpioActiveHigh);

  const interval = Number(input.statusIntervalMs);
  if (input.statusIntervalMs === undefined) {
    value.statusIntervalMs = DEFAULT_SETTINGS.statusIntervalMs;
  } else if (!Number.isInteger(interval) || interval < 2000 || interval > 600000) {
    errors.push('statusIntervalMs must be an integer between 2000 and 600000');
  } else {
    value.statusIntervalMs = interval;
  }

  // offlineAfterMs (#36, was config.js-only): how long a node may stay
  // silent before the relay shows it offline and forces its link closed.
  // The floor relative to the status cadence is a hard rule, not a warning:
  // one lost status must not flip a live node to offline.
  const offline = Number(input.offlineAfterMs);
  if (input.offlineAfterMs === undefined) {
    value.offlineAfterMs = DEFAULT_SETTINGS.offlineAfterMs;
  } else if (!Number.isInteger(offline) || offline < 30000 || offline > 3600000) {
    errors.push('offlineAfterMs must be an integer between 30000 and 3600000');
  } else if (offline < 3 * value.statusIntervalMs) {
    errors.push('offlineAfterMs must be at least 3x statusIntervalMs (one lost status must not mark a node offline)');
  } else {
    value.offlineAfterMs = offline;
  }

  // minLogLevel (#36, was a fixed "info"): the lowest level nodes queue for
  // the relay — pushed to every node with the settings message.
  if (input.minLogLevel === undefined) {
    value.minLogLevel = DEFAULT_SETTINGS.minLogLevel;
  } else if (input.minLogLevel === 'info' || input.minLogLevel === 'warn' || input.minLogLevel === 'error') {
    value.minLogLevel = input.minLogLevel;
  } else {
    errors.push('minLogLevel must be one of: info, warn, error');
  }

  if (input.names !== undefined) {
    if (input.names === null || typeof input.names !== 'object' || Array.isArray(input.names)) {
      errors.push('names must be an object of deviceId -> name');
    } else {
      for (const [id, name] of Object.entries(input.names)) {
        if (!/^[A-Za-z0-9._-]{1,64}$/.test(id)) {
          errors.push(`names: invalid device id "${id}"`);
          continue;
        }
        if (typeof name !== 'string' || name.length === 0 || name.length > 40) {
          errors.push(`names: "${id}" needs a name of 1-40 characters`);
          continue;
        }
        value.names[id] = name;
      }
    }
  }

  if (errors.length > 0) return { ok: false, errors };
  return { ok: true, value, warnings };
}

// Loads settings from disk (or defaults when missing/corrupt) and validates
// what it found — a hand-edited file must pass the same rules as the form.
// `existed` says whether a settings file was there: the relay keeps its
// config.js/env values in effect until the operator saves for the first
// time (that is what makes env overrides work, see server.js).
function loadSettings(file, readJsonFileFn) {
  let stored = null;
  try {
    stored = readJsonFileFn(file, null);
  } catch (e) {
    return {
      settings: { ...DEFAULT_SETTINGS },
      warnings: [`settings file ${file} could not be read (${e.message}) — defaults are in effect`],
      existed: false,
    };
  }
  if (stored === null) return { settings: { ...DEFAULT_SETTINGS }, warnings: [], existed: false };
  const result = validateSettings(stored);
  if (!result.ok) {
    return {
      settings: { ...DEFAULT_SETTINGS },
      warnings: [`settings file ${file} was invalid (${result.errors.join('; ')}) — defaults are in effect`],
      existed: true,
    };
  }
  return { settings: result.value, warnings: [], existed: true };
}

// Thin IO wrapper: everything above is pure, this is the only side effect.
class SettingsStore {
  constructor(file, readJsonFileFn) {
    this.file = file;
    const { settings, warnings, existed } = loadSettings(file, readJsonFileFn);
    this.settings = settings;
    this.warnings = warnings;
    this.existed = existed;
  }

  // Validates, stores and returns { ok, value | errors, warnings }.
  save(input, writeFileAtomicFn = writeFileAtomic) {
    const result = validateSettings(input);
    if (!result.ok) return result;
    try {
      writeFileAtomicFn(this.file, JSON.stringify(result.value, null, 2));
    } catch (e) {
      return { ok: false, errors: [`could not write settings: ${e.message}`] };
    }
    this.settings = result.value;
    return { ok: true, value: result.value, warnings: result.warnings };
  }

  get() {
    return { ...this.settings, names: { ...this.settings.names } };
  }

  nameFor(deviceId) {
    return this.settings.names[deviceId] || deviceId;
  }
}

module.exports = {
  DEFAULT_SETTINGS,
  RISKY_GPIO_PINS,
  isGpioPinAllowed,
  validateSettings,
  loadSettings,
  SettingsStore,
};
