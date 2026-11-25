'use strict';

// Copy this file to config.js and edit the values for your install.
// (config.js is gitignored; env vars PORT / NODE_PORT / RELAY_KEY / NODE_KEY
// override each value, which is how the e2e tests run a second relay.)

module.exports = {
  // HTTP API port for the PHP frontend (and the OTA endpoints the nodes use).
  port: 8080,
  // Raw TCP port the ESP nodes connect to.
  nodePort: 7777,
  // Shared secret the PHP frontend must send as X-API-Key on every /api call.
  // Generate one: node -e "console.log(require('crypto').randomBytes(24).toString('hex'))"
  relayKey: 'CHANGE-ME-relay-key',
  // Shared secret each node must send in its auth line. Each node stores
  // it in its own flash via its setup page (task #140) - nothing to
  // compile into a sketch: the end user flashes a binary and types this
  // key (with the relay's address) on the node's setup page.
  nodeKey: 'CHANGE-ME-node-key',
  // Show a node offline after this long without any message. Must be at
  // least 3x the sketch's STATUS_INTERVAL_MS (settings.statusIntervalMs).
  // Since #36 the dashboard's settings page owns this value (with the 3x
  // rule enforced on save); this entry only seeds the default until the
  // first settings save.
  offlineAfterMs: 45000,
};
