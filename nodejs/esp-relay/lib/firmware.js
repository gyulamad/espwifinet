'use strict';

// firmware.js — the relay half of OTA. The dashboard uploads a .bin built
// from the Arduino sketch; nodes ask "which version is current?" on the
// /firmware/version HTTP endpoint and download /firmware/node.bin when it
// differs from theirs. This module owns the decision logic (is the upload a
// plausible firmware image, which version is newer, what does the version
// endpoint reply); server.js owns the HTTP wiring.
//
// The version reply shape matches espcamsys2's: "<version>\n<md5>\n" — the
// md5 lets the node (and the operator) check the download without trusting
// HTTP headers.

const fs = require('fs');
const crypto = require('crypto');

// ESP8266 OTA images start with the firmware header's magic byte 0xE9 and
// the flash-size/flags bytes that follow. Anything else would brick the node
// at the next Update.begin() — refuse it at upload time, not at flash time.
function isPlausibleEsp8266Image(buf) {
  if (!buf || buf.length < 16) return false;
  return buf[0] === 0xe9;
}

// Versions are dotted integers ("1.0.3"). Non-numeric parts fall back to
// string comparison; equal shapes compare numerically. Returns a positive
// number when a > b, negative when a < b, 0 when equal.
function compareVersions(a, b) {
  const pa = String(a).split('.');
  const pb = String(b).split('.');
  for (let i = 0; i < Math.max(pa.length, pb.length); i++) {
    const na = /^\d+$/.test(pa[i] || '') ? Number(pa[i]) : NaN;
    const nb = /^\d+$/.test(pb[i] || '') ? Number(pb[i]) : NaN;
    if (Number.isFinite(na) && Number.isFinite(nb)) {
      if (na !== nb) return na - nb;
    } else {
      const sa = pa[i] || '';
      const sb = pb[i] || '';
      if (sa !== sb) return sa < sb ? -1 : 1;
    }
  }
  return 0;
}

// True when the node should download: the served version differs from the
// node's (either direction counts — a rollback is as legitimate as an update;
// the operator decides by uploading the bin).
function shouldUpdate(nodeVersion, servedVersion) {
  if (typeof servedVersion !== 'string' || servedVersion === '') return false;
  if (typeof nodeVersion !== 'string' || nodeVersion === '') return true;
  return compareVersions(nodeVersion, servedVersion) !== 0;
}

// Stores an uploaded image: writes it atomically, bumps the version pointer
// file. Returns { ok, version, md5, size } or { ok: false, errors }.
function storeFirmware(firmwareDir, imageBuffer, version, writeFileAtomicFn) {
  const errors = [];
  if (!imageBuffer || !isPlausibleEsp8266Image(imageBuffer)) {
    errors.push('upload does not look like an ESP8266 firmware image (missing 0xE9 header)');
  }
  if (typeof version !== 'string' || !/^[A-Za-z0-9._-]{1,32}$/.test(version)) {
    errors.push('version must be 1-32 characters of [A-Za-z0-9._-]');
  }
  if (errors.length > 0) return { ok: false, errors };

  try {
    writeFileAtomicFn(`${firmwareDir}/node.bin`, imageBuffer);
    writeFileAtomicFn(`${firmwareDir}/version.txt`, `${version}\n`);
    writeFileAtomicFn(`${firmwareDir}/md5.txt`, crypto.createHash('md5').update(imageBuffer).digest('hex'));
  } catch (e) {
    return { ok: false, errors: [`could not write firmware: ${e.message}`] };
  }
  return { ok: true, version, md5: crypto.createHash('md5').update(imageBuffer).digest('hex'), size: imageBuffer.length };
}

// Reads the current version/md5 pair for the /firmware/version endpoint.
// Missing files mean "nothing uploaded yet" — the node then keeps its own
// firmware, which is the correct behaviour for a fresh install.
function currentFirmware(firmwareDir) {
  const read = (f) => {
    try {
      return fs.readFileSync(`${firmwareDir}/${f}`, 'utf8').trim();
    } catch (e) {
      return '';
    }
  };
  const version = read('version.txt');
  if (version === '') return { version: null, md5: null };
  const md5 = read('md5.txt') || null;
  return { version, md5 };
}

// The exact body the version endpoint serves: "<version>\n<md5>\n".
function versionReply(firmwareDir) {
  const { version, md5 } = currentFirmware(firmwareDir);
  if (version === null) return '';
  return `${version}\n${md5 || ''}\n`;
}

module.exports = {
  isPlausibleEsp8266Image,
  compareVersions,
  shouldUpdate,
  storeFirmware,
  currentFirmware,
  versionReply,
};
