'use strict';

// devices.js — the relay's picture of the fleet: who is connected, what they
// told us about themselves, and how the dashboard's snapshot is built from it.
//
// A node is ONLINE when it has a live TCP connection AND it has spoken within
// offlineAfterMs (its status messages come every statusIntervalMs; a node that
// went silent without closing the socket — weak WiFi, half-open NAT mapping —
// must not be shown as online forever). Two connections claiming the same
// deviceId are resolved the way espcamsys2 does it: the newer one wins, the
// older one is classified 'stale' (silent past the live window: the same board
// reconnecting) or 'conflict' (still talking: two boards share one id — both
// are closed and the id must be fixed by the operator).

const LIVE_WINDOW_MS = 2000;

// True when `silentMs` (how long since the older connection last delivered a
// message) still looks like a second live sender rather than a stale leftover.
function isConnectionConflict(silentMs, liveWindowMs = LIVE_WINDOW_MS) {
  return silentMs < liveWindowMs;
}

// The per-device record the relay keeps in memory. `now` and everything the
// node sent arrive as plain values so this stays unit-testable.
function createDeviceInfo(deviceId, now) {
  return {
    deviceId,
    online: true,
    connectedAt: now,
    lastSeen: now,
    // what the node reported (all optional until hello/status arrive)
    fwVersion: null,
    chip: null,
    ip: null,
    ssid: null,
    rssi: null,
    heapFree: null,
    uptimeMs: null,
    resetReason: null,
    gpioPin: null,      // pin the node says it is actually driving
    gpioState: null,    // 'on' | 'off' as the node last acknowledged
    // relay-side bookkeeping
    connectCount: 0,
    lastDisconnectReason: null,
    droppedMessages: 0,
  };
}

// Applies a status message's fields onto the device record. Unknown/absent
// fields are left unchanged; nothing here trusts types beyond a String()/
// Number() coercion with bounds.
function applyStatusMessage(dev, msg) {
  if (typeof msg.ssid === 'string' && msg.ssid.length <= 32) dev.ssid = msg.ssid;
  if (Number.isFinite(msg.rssi) && msg.rssi >= -100 && msg.rssi <= 0) dev.rssi = Math.round(msg.rssi);
  if (Number.isFinite(msg.heapFree) && msg.heapFree >= 0) dev.heapFree = Math.round(msg.heapFree);
  if (Number.isFinite(msg.uptimeMs) && msg.uptimeMs >= 0) dev.uptimeMs = Math.round(msg.uptimeMs);
  if (Number.isInteger(msg.gpioPin) && msg.gpioPin >= 0 && msg.gpioPin <= 16) dev.gpioPin = msg.gpioPin;
  if (msg.gpioState === 'on' || msg.gpioState === 'off') dev.gpioState = msg.gpioState;
}

// Applies hello's fields (firmware identity, reported once after connect and
// after every OTA reboot).
function applyHelloMessage(dev, msg) {
  if (typeof msg.fw === 'string' && msg.fw.length <= 32) dev.fwVersion = msg.fw;
  if (typeof msg.chip === 'string' && msg.chip.length <= 32) dev.chip = msg.chip;
  applyStatusMessage(dev, msg);
}

// RSSI -> the 0-4 bar count the status page shows next to the signal.
// (dBm bands: the usual consumer mapping, good enough for a dashboard.)
function rssiToBars(rssi) {
  if (!Number.isFinite(rssi)) return 0;
  if (rssi >= -55) return 4;
  if (rssi >= -67) return 3;
  if (rssi >= -78) return 2;
  if (rssi >= -88) return 1;
  return 0;
}

// Builds the array the /api/status endpoint returns: one row per known
// device, offline ones included (the operator should see that a node exists
// but is down, not just an empty table). Rows are sorted by device id for a
// stable, diffable order; `nameFor` resolves friendly names from settings.
function buildSnapshot(devicesMap, nameFor, now, offlineAfterMs) {
  const rows = [];
  for (const dev of devicesMap.values()) {
    const online = dev.online && (now - dev.lastSeen) < offlineAfterMs;
    rows.push({
      deviceId: dev.deviceId,
      name: nameFor(dev.deviceId),
      online,
      lastSeenAgoMs: online ? 0 : Math.max(0, now - dev.lastSeen),
      connectedAt: dev.connectedAt,
      fwVersion: dev.fwVersion,
      chip: dev.chip,
      ip: dev.ip,
      ssid: dev.ssid,
      rssi: dev.rssi,
      signalBars: rssiToBars(dev.rssi),
      heapFree: dev.heapFree,
      uptimeMs: dev.uptimeMs,
      resetReason: dev.resetReason,
      gpioPin: dev.gpioPin,
      gpioState: dev.gpioState,
      connectCount: dev.connectCount,
      lastDisconnectReason: dev.lastDisconnectReason,
    });
  }
  rows.sort((a, b) => (a.deviceId < b.deviceId ? -1 : a.deviceId > b.deviceId ? 1 : 0));
  return rows;
}

module.exports = {
  LIVE_WINDOW_MS,
  isConnectionConflict,
  createDeviceInfo,
  applyStatusMessage,
  applyHelloMessage,
  rssiToBars,
  buildSnapshot,
};
