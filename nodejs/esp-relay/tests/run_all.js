'use strict';

// run_all.js — runs every unit test file in tests/node/ with plain node.
// No framework: each test file exports run() and reports its own failures
// with assert. Exit code 1 if anything failed, so CI and run_tests.sh can
// gate on it.

const fs = require('fs');
const path = require('path');

const dir = __dirname;
const files = fs.readdirSync(dir).filter((f) => f.startsWith('test_') && f.endsWith('.js')).sort();

let failed = 0;
for (const f of files) {
  const mod = require(path.join(dir, f));
  try {
    mod.run();
    console.log(`PASS ${f}`);
  } catch (e) {
    failed += 1;
    console.error(`FAIL ${f}: ${e && e.stack ? e.stack : e}`);
  }
}

if (files.length === 0) {
  console.error('no test files found');
  process.exit(1);
}
if (failed > 0) {
  console.error(`\n${failed} of ${files.length} test files FAILED`);
  process.exit(1);
}
console.log(`\nall ${files.length} unit test files passed`);
