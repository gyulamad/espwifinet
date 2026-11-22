'use strict';

// gpiostate.js — the one dashboard toggle's desired state ("on"/"off" for the
// same GPIO leg on every node) and the rules for changing it. The desired
// state is persisted, so a node that was offline during a toggle gets the
// current state the moment it reconnects — the dashboard never has to
// remember which nodes were reachable.
//
// The relay does not track per-node "which toggle command did you already
// get" queues: it simply re-sends the full desired state on every reconnect
// and on every save. The message is 30 bytes and idempotent (the node sets
// the pin to the given level), which is far simpler and cannot desync.

const { writeFileAtomic } = require('./util');

// Only these two strings are ever a valid desired state — anything else from
// the HTTP API is a 400, not a guess.
const GPIO_STATES = ['on', 'off'];
const DEFAULT_GPIO_STATE = 'off';

function isGpioStateValid(state) {
  return GPIO_STATES.includes(state);
}

// Persists and returns the new state, or { ok:false, errors }.
function saveGpioState(file, state, readJsonFileFn, writeFileAtomicFn = writeFileAtomic) {
  if (!isGpioStateValid(state)) {
    return { ok: false, errors: [`state must be one of: ${GPIO_STATES.join(', ')}`] };
  }
  try {
    writeFileAtomicFn(file, JSON.stringify({ gpioState: state }, null, 2));
  } catch (e) {
    return { ok: false, errors: [`could not write state: ${e.message}`] };
  }
  return { ok: true, value: state };
}

// Loads the persisted desired state (default 'off' for a fresh install, so a
// rebuilt Pi never powers the relay pins on by surprise).
function loadGpioState(file, readJsonFileFn) {
  const stored = readJsonFileFn(file, null);
  if (!stored || !isGpioStateValid(stored.gpioState)) return DEFAULT_GPIO_STATE;
  return stored.gpioState;
}

module.exports = {
  GPIO_STATES,
  DEFAULT_GPIO_STATE,
  isGpioStateValid,
  saveGpioState,
  loadGpioState,
};
