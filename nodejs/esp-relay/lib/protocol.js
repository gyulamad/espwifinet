'use strict';

// protocol.js — pure parsing/building for the node push connection's wire
// protocol, so the framing rules can be unit tested with hand-built buffers
// instead of a live TCP connection (same split as espcamsys2's protocol.js:
// server.js owns sockets, this file owns the rules).
//
// Wire protocol (both directions, one TCP connection per node):
//   1. one line, sent by the node right after connect:
//          "<deviceId>\t<apiKey>\n"            (auth, sent once)
//   2. repeated both ways:
//          [4-byte big-endian length][JSON payload]
//
// A JSON payload is always an object with a string `type` field; see
// classifyMessage() for the accepted types. Keeping every message a small
// JSON object (instead of binary structs) costs a few bytes per message and
// buys debuggability: any telnet/netcat session can be read by hand.

// Messages larger than this are treated as protocol errors rather than
// buffered forever. Status/log messages are a few hundred bytes; 64 KB
// leaves two orders of magnitude of headroom.
const MAX_MESSAGE_BYTES = 64 * 1024;

// The auth line has two short tokens separated by a tab; anything longer is
// malformed (or hostile) and must not be buffered indefinitely.
const MAX_AUTH_LINE_BYTES = 256;

// Index of the '\n' (0x0a) terminating the one-time auth line, or -1 when
// the buffer does not hold a complete line yet.
function findAuthLineEnd(buf) {
  return buf.indexOf(0x0a);
}

// True once an un-terminated auth line has grown past any plausible real
// one: the caller should drop the connection instead of buffering forever.
function isAuthLineTooLong(bufLength, maxLength = MAX_AUTH_LINE_BYTES) {
  return bufLength > maxLength;
}

// Splits one raw auth line buffer (without the trailing newline) into
// { deviceId, apiKey }, trimming a trailing '\r' for CRLF senders.
// Returns null unless both parts are non-empty and free of control bytes.
function parseAuthLine(lineBuf) {
  const line = lineBuf.toString('utf8').replace(/\r$/, '');
  const tab = line.indexOf('\t');
  if (tab === -1) return null;
  const deviceId = line.slice(0, tab);
  const apiKey = line.slice(tab + 1);
  if (!deviceId || !apiKey) return null;
  if (/[\x00-\x1f\x7f]/.test(deviceId)) return null;
  return { deviceId, apiKey };
}

// Device ids are used as file names (logs/<id>.log) and object keys, so they
// are restricted to a conservative charset up front — at connect time, before
// anything is stored or echoed anywhere.
function isDeviceIdValid(deviceId) {
  return typeof deviceId === 'string'
    && deviceId.length >= 1
    && deviceId.length <= 64
    && /^[A-Za-z0-9._-]+$/.test(deviceId);
}

// True when a declared message length is inside the sanity cap. A length of
// 0 is also refused: every message is a JSON object, so it can never be empty.
function isMessageLengthValid(len, maxLen = MAX_MESSAGE_BYTES) {
  return len > 0 && len <= maxLen;
}

// Encodes one outgoing message object as [4-byte big-endian length][JSON].
// Returns null when the object cannot be stringified or would exceed the cap,
// so the caller can log-and-skip instead of throwing mid-loop.
function encodeMessage(obj, maxLen = MAX_MESSAGE_BYTES) {
  if (!obj || typeof obj !== 'object') return null;
  let json;
  try {
    json = JSON.stringify(obj);
  } catch (e) {
    return null;
  }
  if (Buffer.byteLength(json) > maxLen) return null;
  const header = Buffer.alloc(4);
  header.writeUInt32BE(Buffer.byteLength(json), 0);
  return Buffer.concat([header, Buffer.from(json, 'utf8')]);
}

// Drains every complete [4-byte length][payload] message buffered in `buf`.
// Returns the raw payloads in order plus whatever is left over (a partial
// length prefix or a partial payload). Throws with code INVALID_MESSAGE_LENGTH
// when a length prefix fails isMessageLengthValid(); the caller must treat
// that as a protocol error and close the connection.
function drainMessages(buf, maxLen = MAX_MESSAGE_BYTES) {
  const messages = [];
  let rest = buf;
  for (;;) {
    if (rest.length < 4) break;
    const len = rest.readUInt32BE(0);
    if (!isMessageLengthValid(len, maxLen)) {
      const err = new Error(`invalid message length: ${len}`);
      err.code = 'INVALID_MESSAGE_LENGTH';
      throw err;
    }
    if (rest.length < 4 + len) break; // message not fully arrived yet
    messages.push(rest.slice(4, 4 + len));
    rest = rest.slice(4 + len);
  }
  return { messages, rest };
}

// Message types a node may send. Anything else is dropped (and counted), so
// a buggy or hostile node cannot make the relay act on unknown verbs.
const NODE_MESSAGE_TYPES = ['hello', 'status', 'log', 'gpio_ack', 'pong'];

// Parses one raw payload into { type, ...fields }, or null when it is not a
// JSON object with a known type. All field validation beyond `type` happens
// in the message-specific handlers — this only classifies.
function classifyMessage(payload) {
  if (payload.length === 0 || payload.length > MAX_MESSAGE_BYTES) return null;
  let obj;
  try {
    obj = JSON.parse(payload.toString('utf8'));
  } catch (e) {
    return null;
  }
  if (!obj || typeof obj !== 'object' || Array.isArray(obj)) return null;
  if (typeof obj.type !== 'string') return null;
  if (!NODE_MESSAGE_TYPES.includes(obj.type)) return null;
  return obj;
}

module.exports = {
  MAX_MESSAGE_BYTES,
  MAX_AUTH_LINE_BYTES,
  NODE_MESSAGE_TYPES,
  findAuthLineEnd,
  isAuthLineTooLong,
  parseAuthLine,
  isDeviceIdValid,
  isMessageLengthValid,
  encodeMessage,
  drainMessages,
  classifyMessage,
};
