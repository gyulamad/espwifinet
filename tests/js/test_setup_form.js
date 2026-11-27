#!/usr/bin/env node
// #177 — the setup form script must keep the Show/Hide toggle working on
// rows added through the "Add another network" button.
//
// Root cause this test pins: the Add handler builds the new row with
// cloneNode(true), and cloneNode copies attributes and children but NEVER
// an addEventListener listener. Before #177 the clone carried its wrapper
// span and a Show button that looked right (resetToggle even re-labelled
// it) but had no click handler — only the rows the page was born with
// could reveal their password box.
//
// The script under test is the REAL script served by the firmware: it is
// extracted from setup_logic.h's R"js(...)js" raw string at test time (the
// same extraction run_tests.sh lints for syntax). The DOM is a minimal
// stub implementing exactly the surface the script uses: simple selectors
// (tag, #id, .class, tag.class, tag[attr=value]), classList, values,
// appendChild/insertBefore/removeChild, cloneNode(true) — which must NOT
// copy listeners, that is the DOM semantic the bug hides behind — and
// addEventListener with a click() dispatch helper.

'use strict';
const fs = require('fs');
const path = require('path');

const HEADER = path.join(__dirname, '..', '..', 'arduino', 'ESP_node', 'setup_logic.h');

// ── extract the real script ──────────────────────────────────────────────
const h = fs.readFileSync(HEADER, 'utf8');
const m = h.match(/R"js\(([\s\S]*?)\)js"/);
if (!m) { console.error('FAIL: the setup form script was not found in setup_logic.h'); process.exit(1); }
const SCRIPT = m[1];

// ── minimal DOM stub ─────────────────────────────────────────────────────

let failed = 0;
function check(cond, what) {
  if (cond) { console.log('ok - ' + what); }
  else { failed++; console.error('FAIL - ' + what); }
}

class ClassList {
  constructor(el) { this._el = el; }
  contains(c) { return this._el.classes.has(c); }
  add(c) { this._el.classes.add(c); }
  remove(c) { this._el.classes.delete(c); }
}

class El {
  constructor(tag, attrs = {}) {
    this.tag = String(tag).toLowerCase();
    this.id = attrs.id || '';
    this.classes = new Set(String(attrs.class || '').split(/\s+/).filter(Boolean));
    const own = Object.assign({}, attrs);
    const kids = Array.isArray(own.children) ? own.children : [];
    delete own.children;
    this.attrs = own;
    this.children = [];
    for (const k of kids) this.appendChild(k);
    this.parentNode = null;
    this.listeners = {};          // type -> [fn]; cloneNode must NOT copy these
    this.type = attrs.type || '';
    this.name = attrs.name || '';
    this.value = attrs.value || '';
    this.checked = !!attrs.checked;
    this.textContent = '';
    this._classList = new ClassList(this);
  }
  get classList() { return this._classList; }
  get className() { return Array.from(this.classes).join(' '); }
  set className(v) { this.classes = new Set(String(v).split(/\s+/).filter(Boolean)); }
  setAttribute(k, v) { this.attrs[k] = String(v); }
  getAttribute(k) { return this.attrs[k]; }
  addEventListener(type, fn) {
    (this.listeners[type] = this.listeners[type] || []).push(fn);
  }
  focus() { DOCUMENT.lastFocused = this; }
  appendChild(child) {
    if (child.parentNode) child.parentNode.removeChild(child);
    child.parentNode = this;
    this.children.push(child);
    return child;
  }
  insertBefore(child, ref) {
    if (child.parentNode) child.parentNode.removeChild(child);
    child.parentNode = this;
    const i = ref ? this.children.indexOf(ref) : -1;
    if (i < 0) this.children.push(child);
    else this.children.splice(i, 0, child);
    return child;
  }
  removeChild(child) {
    const i = this.children.indexOf(child);
    if (i >= 0) this.children.splice(i, 1);
    child.parentNode = null;
    return child;
  }
  cloneNode(deep) {
    // DOM semantics: attributes and (when deep) children are copied; event
    // listeners are NOT. The #177 bug lives exactly behind this honesty.
    const c = new El(this.tag, Object.assign({}, this.attrs, {
      id: this.id, class: Array.from(this.classes).join(' '),
      type: this.type, name: this.name, value: this.value,
      checked: this.checked,
    }));
    c.textContent = this.textContent;
    if (deep) for (const ch of this.children) c.appendChild(ch.cloneNode(true));
    return c;
  }
  matches(selector) {
    // the script only uses simple selectors: tag, #id, .class, tag.class,
    // tag[attr=value]
    const attrs = [];
    const rest = selector.replace(/\[([^\]]*)\]/g, (full, inner) => { attrs.push(inner); return ' '; });
    let tag = null, ok = true;
    for (const token of rest.split(/\s+/).filter(Boolean)) {
      let t = token;
      const tm = t.match(/^([a-zA-Z][\w-]*)/);
      if (tm) { tag = tm[0].toLowerCase(); t = t.slice(tm[0].length); }
      while (t.length) {
        const seg = t.match(/^[.#][\w-]+/);
        if (!seg) return false;
        if (seg[0][0] === '#') ok = ok && this.id === seg[0].slice(1);
        else ok = ok && this.classes.has(seg[0].slice(1));
        t = t.slice(seg[0].length);
      }
    }
    if (tag && this.tag !== tag) return false;
    for (const inner of attrs) {
      const eq = inner.indexOf('=');
      if (eq < 0) { if (this.attrs[inner] === undefined) return false; }
      else if (String(this[inner.slice(0, eq)]) !== inner.slice(eq + 1)) return false;
    }
    return ok;
  }
  querySelectorAll(selector) {
    const out = [];
    for (const ch of this.children) {
      if (ch.matches(selector)) out.push(ch);
      out.push(...ch.querySelectorAll(selector));
    }
    return out;
  }
  querySelector(selector) { return this.querySelectorAll(selector)[0] || null; }
}

const documentEl = new El('#document');
const DOCUMENT = {
  lastFocused: null,
  createElement(tag) { return new El(tag); },
  querySelector(sel) { return documentEl.querySelector(sel); },
  querySelectorAll(sel) { return documentEl.querySelectorAll(sel); },
  getElementById(id) {
    const found = documentEl.querySelectorAll('#' + id);
    return found[0] || null;
  },
};

function click(el) {
  // a real click bubbles: the Remove handler sits on the fieldset, not on
  // the .rm button, so walk the ancestor chain and run every click listener
  // with ev.target staying the clicked element.
  for (let node = el; node; node = node.parentNode) {
    const fns = (node.listeners.click || []).slice();
    for (const fn of fns) fn({ target: el, classList: el.classList });
  }
}

// ── fixture: the page as renderForm()/netRowHtml() render it ────────────
// (mirrors arduino/ESP_node/setup_logic.h: two .net rows, the addwrap/addrow
// pair, and the relay fieldset whose r.key box also rides the toggle)

function netRow(index, opts = {}) {
  return new El('div', { class: 'net', children: [
    new El('input', { type: 'text', class: 'ssid', name: 'w.ssid.' + index, maxlength: '32',
                      value: opts.ssid || '', placeholder: 'network name ' + index }),
    new El('input', { type: 'password', class: 'pass', name: 'w.pass.' + index,
                      autocomplete: 'new-password',
                      placeholder: opts.hasPassword ? 'password set - blank keeps it' : 'password' }),
    new El('label', { children: [
      new El('input', { type: 'checkbox', class: 'open', name: 'w.open.' + index, value: '1' }),
    ]}),
    new El('button', { type: 'button', class: 'rm' }),
  ]});
}

const wifiFieldset = new El('fieldset', { children: [
  netRow(1, { ssid: 'Home', hasPassword: true }),
  netRow(2, {}),
  new El('div', { id: 'addwrap', children: [
    new El('button', { type: 'button', id: 'addrow' }),
  ]}),
]});
const relayFieldset = new El('fieldset', { children: [
  new El('input', { type: 'text', name: 'r.host', maxlength: '64', value: '' }),
  new El('input', { type: 'text', name: 'r.port', maxlength: '5', value: '' }),
  new El('input', { type: 'password', class: 'pass', name: 'r.key', autocomplete: 'new-password' }),
]});
documentEl.appendChild(wifiFieldset);
documentEl.appendChild(relayFieldset);

// ── run the real script ──────────────────────────────────────────────────
try { new Function('document', SCRIPT)(DOCUMENT); }
catch (e) { console.error('FAIL: the setup form script threw: ' + e.message); process.exit(1); }

function showButtonOf(input) {
  return input.parentNode.querySelector('.showpw');
}
function rows() { return wifiFieldset.querySelectorAll('.net'); }

// ── 1. page load: every password box is wrapped and armed ────────────────
const bornPws = documentEl.querySelectorAll('input[type=password]');
check(bornPws.length === 3, 'the page was born with 3 password boxes (2 WiFi rows + r.key), found ' + bornPws.length);
let allWrapped = true, allArmed = true;
for (const pw of bornPws) {
  if (!pw.parentNode.matches('.pw')) allWrapped = false;
  const btn = showButtonOf(pw);
  if (!btn || btn.textContent !== 'Show' || btn.getAttribute('aria-pressed') !== 'false') allArmed = false;
}
check(allWrapped, 'every born password box sits in a .pw wrapper');
check(allArmed, 'every born box has a Show button at aria-pressed=false');

// ── 2. a born row toggles (the #171 behavior, unchanged) ─────────────────
const row1pw = wifiFieldset.querySelectorAll('input.pass')[0];
click(showButtonOf(row1pw));
check(row1pw.type === 'text' && showButtonOf(row1pw).textContent === 'Hide' &&
      showButtonOf(row1pw).getAttribute('aria-pressed') === 'true',
      'clicking a born Show button reveals the box (type=text, Hide, aria-pressed=true)');
click(showButtonOf(row1pw));
check(row1pw.type === 'password' && showButtonOf(row1pw).textContent === 'Show',
      'clicking Hide re-hides a born box');

// ── 3. Add a row: THE #177 REGRESSION ────────────────────────────────────
const addrow = DOCUMENT.getElementById('addrow');
click(addrow);
check(rows().length === 3, 'the Add button appends a third row');
check(rows()[2].querySelector('input.ssid').name === 'w.ssid.3' &&
      rows()[2].querySelector('input.pass').name === 'w.pass.3' &&
      rows()[2].querySelector('input.open').name === 'w.open.3',
      'the added row is renumbered (w.ssid.3/w.pass.3/w.open.3)');
check(DOCUMENT.lastFocused === rows()[2].querySelector('input.ssid'),
      'focus lands on the added row\'s SSID box');

const addedPw = rows()[2].querySelector('input.pass');
check(addedPw.value === '' && addedPw.type === 'password',
      'the added row starts blank and hidden (resetToggle still holds)');
const addedBtn = showButtonOf(addedPw);
check(!!addedBtn && addedBtn.textContent === 'Show', 'the added row carries a Show-labelled button');
click(addedBtn);
check(addedPw.type === 'text' && addedBtn.textContent === 'Hide',
      'THE #177 FIX: the ADDED row\'s Show button reveals its box');
click(addedBtn);
check(addedPw.type === 'password' && addedBtn.textContent === 'Show',
      'the added row\'s button toggles back to hidden');

// ── 4. cloning a REVEALED row still resets and re-arms the clone ─────────
click(showButtonOf(row1pw));            // reveal row 1
click(addrow);                          // clone it while revealed
const cloneRow = rows()[3];             // the newest row sits just above addwrap
const clone2pw = cloneRow.querySelector('input.pass');
check(clone2pw.type === 'password' && showButtonOf(clone2pw).textContent === 'Show',
      'cloning a revealed row still starts the clone hidden (resetToggle)');
click(showButtonOf(clone2pw));
check(clone2pw.type === 'text', 'the clone of a revealed row still toggles (re-bind)');
click(showButtonOf(row1pw));            // put row 1 back to hidden
click(showButtonOf(clone2pw));

// ── 5. relay key box (not inside .net) toggles too ───────────────────────
const relayKey = relayFieldset.querySelector('input.pass');
click(showButtonOf(relayKey));
check(relayKey.type === 'text', 'the relay r.key box reveals (its button was born bound)');

// ── 6. Remove + renumber; clone-of-clone rows stay toggleable ────────────
click(rows()[1].querySelector('.rm'));  // remove the second row
check(rows().length === 3, 'the Remove button removes a row');
check(rows()[0].querySelector('input.ssid').name === 'w.ssid.1' &&
      rows()[1].querySelector('input.ssid').name === 'w.ssid.2' &&
      rows()[2].querySelector('input.ssid').name === 'w.ssid.3',
      'names renumber after a removal (w.ssid.1..3, no gaps)');

click(addrow);                          // add from a row that was itself a clone
click(addrow);                          // add again — the source is a clone of a clone
check(rows().length === 5, 'two more rows added (rows 4 and 5)');
const cloneOfClonePw = rows()[4].querySelector('input.pass');
click(showButtonOf(cloneOfClonePw));
check(cloneOfClonePw.type === 'text',
      'a row cloned from a CLONE still toggles (#177 holds on the clone-of-clone path)');
click(showButtonOf(cloneOfClonePw));

// ── 7. the born r.key listener is not double-bound after all this ────────
click(showButtonOf(relayKey));
check(relayKey.type === 'password', 'r.key toggles back (no duplicated listeners)');

// ── verdict ──────────────────────────────────────────────────────────────
console.log('');
if (failed) {
  console.error('setup form script: ' + failed + ' check(s) FAILED');
  process.exit(1);
}
console.log('setup form script: ALL CHECKS PASSED (Show/Hide survives the Add button, #177)');
