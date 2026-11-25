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

// ESP OTA images (ESP8266 AND ESP32 — the ESP image format is the same for
// both) start with the firmware header's magic byte 0xE9 and the flash-size/
// flags bytes that follow. Anything else would brick the node at the next
// Update.begin() — refuse it at upload time, not at flash time. The magic
// alone cannot tell the two boards apart, which is exactly why the images
// are stored and served PER CHIP (see the chip paths below): a mixed fleet
// never downloads the other board's image.
function isPlausibleEspImage(buf) {
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

// The boards the relay stores and serves images for (#131): the node's
// hello reports its chip with these exact tokens (board.h's ESPNET_CHIP_NAME).
const CHIPS = ['esp8266', 'esp32'];

function isKnownChip(chip) {
  return CHIPS.includes(chip);
}

// The file names one chip's image set lives under. esp8266 keeps the
// original names (node.bin / version.txt / md5.txt) so a relay upgraded in
// place keeps serving the nodes it already had; esp32 gets suffixed ones.
function chipFileNames(chip) {
  if (chip === 'esp32') {
    return { bin: 'node-esp32.bin', version: 'version-esp32.txt', md5: 'md5-esp32.txt' };
  }
  return { bin: 'node.bin', version: 'version.txt', md5: 'md5.txt' };
}

// Stores an uploaded image: writes it atomically, bumps the version pointer
// file. chip selects the board's image set ('esp8266' default, 'esp32').
// Returns { ok, version, md5, size } or { ok: false, errors }.
function storeFirmware(firmwareDir, imageBuffer, version, writeFileAtomicFn, chip = 'esp8266') {
  const errors = [];
  if (!isKnownChip(chip)) {
    errors.push(`unknown chip "${chip}" — serve one of: ${CHIPS.join(', ')}`);
  }
  if (!imageBuffer || !isPlausibleEspImage(imageBuffer)) {
    errors.push('upload does not look like an ESP firmware image (missing 0xE9 header)');
  }
  if (typeof version !== 'string' || !/^[A-Za-z0-9._-]{1,32}$/.test(version)) {
    errors.push('version must be 1-32 characters of [A-Za-z0-9._-]');
  }
  if (errors.length > 0) return { ok: false, errors };
  const names = chipFileNames(chip);

  try {
    writeFileAtomicFn(`${firmwareDir}/${names.bin}`, imageBuffer);
    writeFileAtomicFn(`${firmwareDir}/${names.version}`, `${version}\n`);
    writeFileAtomicFn(`${firmwareDir}/${names.md5}`, crypto.createHash('md5').update(imageBuffer).digest('hex'));
  } catch (e) {
    return { ok: false, errors: [`could not write firmware: ${e.message}`] };
  }
  return { ok: true, version, md5: crypto.createHash('md5').update(imageBuffer).digest('hex'), size: imageBuffer.length };
}

// Reads the current version/md5 pair for the /firmware/version endpoint.
// chip selects the board's image set ('esp8266' default, 'esp32'). Missing
// files mean "nothing uploaded yet" — the node then keeps its own firmware,
// which is the correct behaviour for a fresh install.
function currentFirmware(firmwareDir, chip = 'esp8266') {
  const names = chipFileNames(isKnownChip(chip) ? chip : 'esp8266');
  const read = (f) => {
    try {
      return fs.readFileSync(`${firmwareDir}/${f}`, 'utf8').trim();
    } catch (e) {
      return '';
    }
  };
  const version = read(names.version);
  if (version === '') return { version: null, md5: null };
  const md5 = read(names.md5) || null;
  return { version, md5 };
}

// The exact body the version endpoint serves: "<version>\n<md5>\n".
function versionReply(firmwareDir, chip = 'esp8266') {
  const { version, md5 } = currentFirmware(firmwareDir, chip);
  if (version === null) return '';
  return `${version}\n${md5 || ''}\n`;
}

// The image file name one chip's /firmware/... endpoint serves (the ESP8266
// route keeps its original /firmware/node.bin for the deployed fleet).
function imageFileName(chip) {
  return chipFileNames(isKnownChip(chip) ? chip : 'esp8266').bin;
}

module.exports = {
  CHIPS,
  isKnownChip,
  isPlausibleEspImage,
  chipFileNames,
  imageFileName,
  compareVersions,
  shouldUpdate,
  storeFirmware,
  currentFirmware,
  versionReply,
};
