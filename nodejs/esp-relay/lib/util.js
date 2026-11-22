'use strict';

// util.js — small helpers shared across the relay. Nothing here knows about
// sockets or Express; everything is trivially testable.

const fs = require('fs');
const path = require('path');

// Environment override: the value if it is a non-empty string, otherwise the
// fallback. Numbers/booleans are compared as strings on purpose — env vars
// are always strings and a silent type guess hides config mistakes.
function envOr(env, name, fallback) {
  const v = env[name];
  return v === undefined || v === '' ? fallback : v;
}

// A timeout in milliseconds from an environment variable: the value if it is
// a finite positive number, otherwise the default.
function parseTimeoutMs(value, fallback) {
  const ms = Number(value);
  return Number.isFinite(ms) && ms > 0 ? ms : fallback;
}

function errorMessage(e) {
  return e && e.message ? e.message : String(e);
}

// Writes a file atomically: the reader (PHP or the firmware endpoint) never
// sees a half-written JSON/bin file after a crash or power cut on the Pi.
function writeFileAtomic(file, data) {
  const tmp = `${file}.tmp-${process.pid}`;
  fs.writeFileSync(tmp, data);
  fs.renameSync(tmp, file);
}

// Reads and JSON-parses a file, returning `fallback` when it is missing or
// corrupt. A corrupt file is logged by the caller; here we only decide the
// value — a broken state file must not keep the relay from starting.
function readJsonFile(file, fallback = null) {
  let raw;
  try {
    raw = fs.readFileSync(file, 'utf8');
  } catch (e) {
    return fallback;
  }
  try {
    return JSON.parse(raw);
  } catch (e) {
    return fallback;
  }
}

// Ensures a directory exists (mkdir -p for one path).
function ensureDir(dir) {
  fs.mkdirSync(dir, { recursive: true });
}

// Strips anything that is not safe in a file name component. Used only for
// ids that already passed isDeviceIdValid(), as a second line of defence
// before the value is ever concatenated into a path.
function safeFileComponent(name) {
  return path.basename(String(name)).replace(/[^A-Za-z0-9._-]/g, '_');
}

// Truncates a string to `max` code points (not bytes), for storing
// node-supplied text (log lines, ssid) without unbounded growth.
function truncate(str, max) {
  return Array.from(String(str)).slice(0, max).join('');
}

module.exports = {
  envOr,
  parseTimeoutMs,
  errorMessage,
  writeFileAtomic,
  readJsonFile,
  ensureDir,
  safeFileComponent,
  truncate,
};
