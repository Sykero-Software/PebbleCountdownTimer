// SPDX-License-Identifier: GPL-3.0-only
const test = require('node:test');
const assert = require('node:assert');
const {
  hmsToSeconds, secondsToHms, sanitizeName, timerListToString, stringToTimerList, MAX_TIMERS,
} = require('../src/pkjs/timer_config');

test('hmsToSeconds / secondsToHms round-trip', () => {
  assert.strictEqual(hmsToSeconds(1, 2, 3), 3723);
  assert.strictEqual(hmsToSeconds(0, 5, 0), 300);
  assert.deepStrictEqual(secondsToHms(3723), { h: 1, m: 2, s: 3 });
  assert.deepStrictEqual(secondsToHms(300), { h: 0, m: 5, s: 0 });
  assert.deepStrictEqual(secondsToHms(0), { h: 0, m: 0, s: 0 });
});

test('hmsToSeconds coerces strings and clamps negatives to 0', () => {
  assert.strictEqual(hmsToSeconds('0', '25', '0'), 1500);
  assert.strictEqual(hmsToSeconds(-1, -1, -1), 0);
});

test('sanitizeName strips separators, trims, caps at 31 bytes', () => {
  assert.strictEqual(sanitizeName('  Egg \x1f\x1e timer '), 'Egg  timer');
  assert.strictEqual(sanitizeName('x'.repeat(40)).length, 31);
  assert.strictEqual(sanitizeName(123), '');
});

test('timerListToString serializes name\\x1fseconds, records \\x1e-joined', () => {
  const s = timerListToString([{ name: 'Egg', seconds: 300 }, { name: 'Tea', seconds: 120 }]);
  assert.strictEqual(s, 'Egg\x1f300\x1eTea\x1f120');
});

test('timerListToString skips zero/negative/invalid durations and empty names use ""', () => {
  assert.strictEqual(timerListToString([{ name: 'a', seconds: 0 }, { name: 'b', seconds: 60 }]), 'b\x1f60');
  assert.strictEqual(timerListToString([]), '');
  assert.strictEqual(timerListToString([{ name: '', seconds: 60 }]), '\x1f60');
});

test('timerListToString caps at MAX_TIMERS', () => {
  const many = [];
  for (let i = 0; i < MAX_TIMERS + 5; i++) { many.push({ name: 't' + i, seconds: 60 }); }
  assert.strictEqual(timerListToString(many).split('\x1e').length, MAX_TIMERS);
});

test('stringToTimerList parses back (round-trip)', () => {
  assert.deepStrictEqual(stringToTimerList('Egg\x1f300\x1eTea\x1f120'),
    [{ name: 'Egg', seconds: 300 }, { name: 'Tea', seconds: 120 }]);
  assert.deepStrictEqual(stringToTimerList(''), []);
});
