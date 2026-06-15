// SPDX-License-Identifier: GPL-3.0-only
const test = require('node:test');
const assert = require('node:assert');
const { resendDict } = require('../src/pkjs/config_sync');

function store(obj) { return (k) => (k in obj ? obj[k] : null); }

test('resendDict: never saved (timer_config absent) -> null (do not clobber watch)', () => {
  assert.strictEqual(resendDict(store({})), null);
  assert.strictEqual(resendDict(store({ sort_order: '2' })), null);
});

test('resendDict: saved config -> dict with parsed SortOrder + AutoReturn', () => {
  assert.deepStrictEqual(
    resendDict(store({ timer_config: 'Egg\x1f300\x1eTea\x1f120', sort_order: '1', auto_return: '1' })),
    { TimerConfig: 'Egg\x1f300\x1eTea\x1f120', SortOrder: 1, AutoReturn: 1 });
});

test('resendDict: explicitly-saved empty list ("") IS sent (user cleared all timers)', () => {
  assert.deepStrictEqual(
    resendDict(store({ timer_config: '', sort_order: '0' })),
    { TimerConfig: '', SortOrder: 0, AutoReturn: 0 });
});

test('resendDict: missing/garbage sort_order defaults to 0', () => {
  assert.strictEqual(resendDict(store({ timer_config: 'a\x1f60' })).SortOrder, 0);
  assert.strictEqual(resendDict(store({ timer_config: 'a\x1f60', sort_order: 'x' })).SortOrder, 0);
});

test('resendDict: missing auto_return defaults to 0', () => {
  assert.strictEqual(resendDict(store({ timer_config: 'a\x1f60' })).AutoReturn, 0);
});
