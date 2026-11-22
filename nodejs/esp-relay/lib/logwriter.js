'use strict';

// logwriter.js — appends one JSON line per log entry to a per-device file
// under logs/, and tails them back out for the dashboard's logs page.
//
// JSON lines (rather than plain text) keep the PHP side from parsing ad-hoc
// formats, and make rotation trivial: when a file passes the size cap it is
// renamed to <name>.old (one generation kept — enough for a home setup, and
// the Pi's SD card never fills up with logs).

const fs = require('fs');
const path = require('path');
const { safeFileComponent } = require('./util');

const DEFAULT_MAX_BYTES = 1024 * 1024; // rotate at 1 MB

// Builds the object that gets serialised per line. Kept pure for tests.
function formatLogLine(entry, nowIso) {
  return `${JSON.stringify({
    ts: nowIso,
    level: entry.level || 'info',
    device: entry.device || 'relay',
    msg: entry.msg,
  })}\n`;
}

// Full path of a device's log file. The id already passed isDeviceIdValid(),
// safeFileComponent is the second line of defence before a path is built.
function logFilePath(logDir, deviceId) {
  return path.join(logDir, `${safeFileComponent(deviceId)}.log`);
}

// Rotates file -> file.old when it exceeds maxBytes. Missing file: nothing.
function rotateIfNeeded(file, maxBytes = DEFAULT_MAX_BYTES) {
  let size = 0;
  try {
    size = fs.statSync(file).size;
  } catch (e) {
    return false; // does not exist yet — nothing to rotate
  }
  if (size < maxBytes) return false;
  try {
    fs.renameSync(file, `${file}.old`);
  } catch (e) {
    return false; // rotation failed; appending continues and retries next line
  }
  return true;
}

// Reads the last `lines` entries of a log file as parsed objects, newest
// first. Unparseable lines (hand-edited file, torn write) are skipped —
// the logs page must render even when one line is damaged.
function tailLog(file, lines = 200) {
  let raw;
  try {
    raw = fs.readFileSync(file, 'utf8');
  } catch (e) {
    return [];
  }
  const out = [];
  const all = raw.split('\n');
  for (let i = all.length - 1; i >= 0 && out.length < lines; i--) {
    const line = all[i].trim();
    if (line === '') continue;
    try {
      const obj = JSON.parse(line);
      if (obj && typeof obj === 'object') out.push(obj);
    } catch (e) {
      // skip damaged line
    }
  }
  return out;
}

class LogWriter {
  constructor(logDir, maxBytes = DEFAULT_MAX_BYTES) {
    this.logDir = logDir;
    this.maxBytes = maxBytes;
  }

  // Appends one entry to a device's (or the relay's own) log file.
  append(entry, nowIso = new Date().toISOString()) {
    fs.mkdirSync(this.logDir, { recursive: true });
    const file = logFilePath(this.logDir, entry.device || 'relay');
    rotateIfNeeded(file, this.maxBytes);
    try {
      fs.appendFileSync(file, formatLogLine(entry, nowIso));
      return true;
    } catch (e) {
      return false;
    }
  }

  tail(deviceId, lines = 200) {
    return tailLog(logFilePath(this.logDir, deviceId), lines);
  }

  // Device ids that have a log file on disk, sorted — so the logs page can
  // list devices that logged and then disappeared.
  listDevices() {
    let names = [];
    try {
      names = fs.readdirSync(this.logDir);
    } catch (e) {
      return [];
    }
    return names
      .filter((n) => n.endsWith('.log'))
      .map((n) => n.slice(0, -4))
      .sort();
  }
}

module.exports = {
  DEFAULT_MAX_BYTES,
  formatLogLine,
  logFilePath,
  rotateIfNeeded,
  tailLog,
  LogWriter,
};
