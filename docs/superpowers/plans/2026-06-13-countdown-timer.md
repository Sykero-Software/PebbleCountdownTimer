# Countdown Timer Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build the standalone Pebble watchapp "Countdown timer": timers are configured on the phone (Clay config list of name + HH/MM/SS), the watch shows them sorted most-recently-used-first and can Start/Pause/Reset each, multiple run at once, and a single Wakeup alerts when a timer hits zero even with the app closed.

**Architecture:** Phone-side TypeScript→`tsc`→`src/pkjs` (mirroring TimeStylePebble) renders a Clay config page whose one custom component (`timerList`) edits the timer list; on save it serializes the list to ONE `TimerConfig` CString AppMessage. The watch (C) parses that string, reconciles it against persisted runtime state, renders a `MenuLayer` (rows sorted by `last_used`) with a per-row `ActionMenu`, ticks running timers with a 1 s `app_timer`, persists state, and keeps exactly ONE `wakeup_schedule` armed for the soonest-expiring timer (re-arming on every change and on wakeup launch). Pure logic (string parse/format, sort, soonest-wakeup, state transitions) lives in dependency-free `timer_calc.c/.h` so it is host-gcc unit-testable; the contract-equivalent TS helpers in `timer_config.ts` are tested with `node:test`.

**Tech Stack:** C + Pebble SDK 4.9 (waf/`wscript`), TypeScript 6 + `pebble-clay@^1.0.4`, `node:test`. Reference implementations in this superrepo: `TimeStylePebble/` (TS/Clay setup, custom component `src/ts/config_widget_list.ts`, `wscript` hooks, `tests/`) and `PebbleTrackWorkTime/` (C watchapp, MenuLayer, persist, Clay JS).

**Conventions (from superrepo CLAUDE.md):**
- All work happens inside `PebbleCountdownTimer/` (a private submodule, currently on `master`). Commit each task there, then **immediately bump the gitlink in the superrepo** (`/home/dev/pebble-timetracking`): `git add PebbleCountdownTimer && git commit -m "Bump PebbleCountdownTimer: <what>"`. Pushing needs a YubiKey (not part of these tasks).
- Build with the headless emulator recipe: cold-boot once via `scripts/pebble-emu-boot.sh emery` (run from the superrepo root), then reuse it; prefix pebble commands with `PEBBLE_QEMU_PATH=~/.local/bin/qemu-pebble-headless`. Use **emery** (200px, the user's real watch) and **diorite** for a quick 144px check; never basalt.
- **Surface every emulator screenshot to the user** via the SendUserFile tool.
- After editing `messageKeys`, run `pebble clean && pebble build` (the `MESSAGE_KEY_*` C macros are cached).
- `src/pkjs/*.js` is generated and gitignored — edit `src/ts/*.ts`, never the JS.

**Serialization contract (MUST be identical on both sides — TS `timer_config.ts` and C `timer_calc.c`):**
- A `TimerConfig` value is a CString. Records are separated by **`\x1e`** (RS, 0x1E); the two fields inside a record (`name`, `seconds`) by **`\x1f`** (US, 0x1F): `name \x1f seconds \x1e name \x1f seconds \x1e …`.
- `seconds` is a base-10 integer ≥ 1. `name` has any `\x1e`/`\x1f` stripped and is truncated to **31** bytes. Empty list ⇒ empty string. At most **16** records (`MAX_TIMERS`); extras dropped.
- A second int key, **`SortOrder`** (0 = most-recently-used, 1 = shortest remaining first, 2 = longest remaining first), is sent alongside; the watch persists and applies it.
- **Persistence note:** `persist_write_data` caps at **256 bytes/key** and a `Timer` is ~64 B, so 16 timers do NOT fit in one blob — each timer is stored in its own persist key (`PERSIST_KEY_TIMER_BASE + i`).

---

## Task 1: Project scaffold (build a do-nothing app)

**Files:**
- Create: `PebbleCountdownTimer/package.json`
- Create: `PebbleCountdownTimer/tsconfig.json`
- Create: `PebbleCountdownTimer/wscript`
- Create: `PebbleCountdownTimer/.gitignore`
- Create: `PebbleCountdownTimer/LICENSE` (copy from `../PebbleTrackWorkTime/LICENSE`)
- Create: `PebbleCountdownTimer/src/c/main.c`
- Create: `PebbleCountdownTimer/src/ts/.keep` (placeholder so `src/ts` exists for the wscript check; removed in Task 4)

- [ ] **Step 1: Generate a fresh UUID**

```bash
cd PebbleCountdownTimer
python3 -c "import uuid; print(uuid.uuid4())"
```
Use the printed value as `<UUID>` below. It MUST differ from TimeStyle (`812efd3c-…`) and PTWT (`2B5F824D-…`).

- [ ] **Step 2: Write `package.json`**

```json
{
  "name": "countdown-timer",
  "author": "Sykerö Software",
  "version": "1.0.0",
  "keywords": ["pebble-app"],
  "private": true,
  "dependencies": {
    "pebble-clay": "^1.0.4"
  },
  "pebble": {
    "displayName": "Countdown timer",
    "uuid": "<UUID>",
    "sdkVersion": "3",
    "enableMultiJS": true,
    "targetPlatforms": ["aplite", "basalt", "chalk", "diorite", "emery", "flint", "gabbro"],
    "capabilities": [],
    "watchapp": { "watchface": false },
    "messageKeys": ["TimerConfig", "SortOrder"],
    "resources": { "media": [] }
  },
  "devDependencies": {
    "typescript": "^6.0.3"
  },
  "scripts": {
    "build": "tsc",
    "typecheck": "tsc --noEmit",
    "pretest": "tsc",
    "test": "node --test tests/*.test.js"
  }
}
```

- [ ] **Step 3: Write `tsconfig.json`** (identical to TimeStylePebble's)

```json
{
  "compilerOptions": {
    "target": "ES5",
    "ignoreDeprecations": "6.0",
    "module": "CommonJS",
    "moduleResolution": "node",
    "lib": ["ES2017", "DOM"],
    "rootDir": "src/ts",
    "outDir": "src/pkjs",
    "strict": true,
    "esModuleInterop": true,
    "forceConsistentCasingInFileNames": true,
    "skipLibCheck": true,
    "noEmitOnError": true,
    "removeComments": false,
    "types": []
  },
  "include": ["src/ts/**/*.ts"]
}
```

- [ ] **Step 4: Write `.gitignore`** — copy `../TimeStylePebble/.gitignore` verbatim (it already ignores `build`, `/src/pkjs/*.js`, linked SDK files, `*.pbw`, `node_modules`).

- [ ] **Step 5: Write `wscript`** — copy `../TimeStylePebble/wscript` but make exactly TWO changes:
  1. In `build(ctx)`, **delete** the line `ctx.add_post_fun(inject_companion_app)` and its preceding comment block (no companionApp here).
  2. **Delete** the entire `def inject_companion_app(ctx):` function.
  Keep `compile_typescript` and `patch_clay_for_new_platforms` unchanged. (The `USE_FAKE_TIME`/`SCREENSHOT_FIXTURES` defines lines may stay; they are harmless.)

- [ ] **Step 6: Copy the license**

```bash
cp ../PebbleTrackWorkTime/LICENSE LICENSE
```

- [ ] **Step 7: Write a minimal `src/c/main.c`** (empty window, just so it builds)

```c
#include <pebble.h>

static Window *s_window;

static void init(void) {
  s_window = window_create();
  window_stack_push(s_window, true);
}

static void deinit(void) {
  window_destroy(s_window);
}

int main(void) {
  init();
  app_event_loop();
  deinit();
}
```

- [ ] **Step 8: Create the `src/ts` placeholder** so the wscript `compile_typescript` guard has a dir (Task 4 adds real `.ts` and removes this).

```bash
mkdir -p src/ts && touch src/ts/.keep
```

- [ ] **Step 9: Install deps and build**

```bash
cd /home/dev/pebble-timetracking/PebbleCountdownTimer
npm install
PEBBLE_QEMU_PATH=~/.local/bin/qemu-pebble-headless pebble build
```
Expected: `npm install` pulls `pebble-clay` + `typescript`; `pebble build` ends with `'build' finished successfully` and writes `build/countdown-timer.pbw`. (The `patch_clay_for_new_platforms` hook prints "Patched pebble-clay for flint/gabbro".)

- [ ] **Step 10: Commit (submodule + gitlink)**

```bash
cd /home/dev/pebble-timetracking/PebbleCountdownTimer
git add package.json tsconfig.json wscript .gitignore LICENSE src/c/main.c
git commit -m "Scaffold Countdown timer watchapp (builds empty window)"
cd /home/dev/pebble-timetracking
git add PebbleCountdownTimer && git commit -m "Bump PebbleCountdownTimer: scaffold watchapp"
```

---

## Task 2: TS pure logic — `timer_config.ts` (serialization contract)

**Files:**
- Create: `PebbleCountdownTimer/src/ts/timer_config.ts`
- Test: `PebbleCountdownTimer/tests/timer_config.test.js`

- [ ] **Step 1: Write the failing test** `tests/timer_config.test.js`

```js
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
```

- [ ] **Step 2: Run it, verify it fails**

Run: `cd PebbleCountdownTimer && npm test`
Expected: FAIL — cannot find module `../src/pkjs/timer_config` (not generated yet).

- [ ] **Step 3: Write `src/ts/timer_config.ts`**

```ts
// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Tuomas Airaksinen

// Serialization contract shared with the watch C side (timer_calc.c). Records
// separated by RS (0x1e), fields (name, seconds) by US (0x1f). Keep in sync.
export const MAX_TIMERS = 16;      // matches MAX_TIMERS in src/c/timer_calc.h
export const NAME_MAX = 31;        // matches NAME_LEN in src/c/timer_calc.h
const RS = '\x1e';
const US = '\x1f';

export interface TimerEntry { name: string; seconds: number; }

export function hmsToSeconds(h: any, m: any, s: any): number {
  const hh = Math.max(0, parseInt(h, 10) || 0);
  const mm = Math.max(0, parseInt(m, 10) || 0);
  const ss = Math.max(0, parseInt(s, 10) || 0);
  return hh * 3600 + mm * 60 + ss;
}

export function secondsToHms(sec: any): { h: number; m: number; s: number } {
  let n = Math.max(0, parseInt(sec, 10) || 0);
  const h = Math.floor(n / 3600); n -= h * 3600;
  const m = Math.floor(n / 60); n -= m * 60;
  return { h: h, m: m, s: n };
}

export function sanitizeName(name: any): string {
  if (typeof name !== 'string') { return ''; }
  let out = name.replace(/[\x1e\x1f]/g, '').replace(/^\s+|\s+$/g, '');
  if (out.length > NAME_MAX) { out = out.substring(0, NAME_MAX); }
  return out;
}

export function timerListToString(list: any): string {
  const arr: any[] = Array.isArray(list) ? list : [];
  const recs: string[] = [];
  for (let i = 0; i < arr.length && recs.length < MAX_TIMERS; i++) {
    const e = arr[i] || {};
    const seconds = parseInt(e.seconds, 10);
    if (isNaN(seconds) || seconds < 1) { continue; }
    recs.push(sanitizeName(e.name) + US + seconds);
  }
  return recs.join(RS);
}

export function stringToTimerList(str: any): TimerEntry[] {
  const out: TimerEntry[] = [];
  if (typeof str !== 'string' || str === '') { return out; }
  const recs = str.split(RS);
  for (let i = 0; i < recs.length && out.length < MAX_TIMERS; i++) {
    const fields = recs[i].split(US);
    const seconds = parseInt(fields[1], 10);
    if (isNaN(seconds) || seconds < 1) { continue; }
    out.push({ name: sanitizeName(fields[0]), seconds: seconds });
  }
  return out;
}
```

- [ ] **Step 4: Run the test, verify it passes**

Run: `cd PebbleCountdownTimer && npm test`
Expected: PASS — all `timer_config` tests pass (pretest runs `tsc`, generating `src/pkjs/timer_config.js`).

- [ ] **Step 5: Commit**

```bash
cd /home/dev/pebble-timetracking/PebbleCountdownTimer
git add src/ts/timer_config.ts tests/timer_config.test.js
git commit -m "Add timer config serialization (TS) + tests"
cd /home/dev/pebble-timetracking && git add PebbleCountdownTimer && git commit -m "Bump PebbleCountdownTimer: TS serialization"
```

---

## Task 3: Clay custom component `timerList`

**Files:**
- Create: `PebbleCountdownTimer/src/ts/config_timer_list.ts`

This is a DOM/Clay component (no automated DOM test, same as TimeStyle's `config_widget_list`). It is serialized via `toSource()` and re-eval'd in the config webview, so — like `config_widget_list.ts` — it MUST be self-contained: no module-scope helper references at runtime, **no spread/destructuring**, no TS downlevel helpers; only `this`, locals, instance props stashed in `initialize`, native DOM + array methods. Verification is the `__`/`_this` grep in Step 3 plus the build in Task 4.

- [ ] **Step 1: Write `src/ts/config_timer_list.ts`**

```ts
// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Tuomas Airaksinen

/* Clay custom component "timerList": a variable-length list (max 16) of countdown
   timers, each a name + H/M/S duration, with a remove (x) button and an Add
   button. NO reordering (the watch sorts by most-recently-used). Component value
   is an array of { name: string, seconds: number }. Like config_widget_list.ts,
   this whole object is serialized via toSource() and re-eval'd in the config
   webview, so every function MUST be self-contained: no module-scope refs at
   runtime, no spread/destructuring, no TS downlevel helpers. Native DOM only. */

function timerListInitialize(this: any, _minified: any, _clayConfig: any): void {
  const self = this;
  const root: HTMLElement = self.$element[0];
  const MAX = 16;   // matches MAX_TIMERS in src/c/timer_calc.h

  function clamp(v: number, lo: number, hi: number): number {
    if (isNaN(v)) { return lo; }
    if (v < lo) { return lo; }
    if (v > hi) { return hi; }
    return v;
  }

  function rowHtml(name: string, h: number, m: number, s: number): string {
    // textarea-safe: escape the name into the value attribute
    const safe = ('' + name).replace(/&/g, '&amp;').replace(/"/g, '&quot;')
      .replace(/</g, '&lt;').replace(/>/g, '&gt;');
    return '<div class="tl-row">' +
      '<input class="tl-name" type="text" placeholder="Name" value="' + safe + '">' +
      '<div class="tl-dur">' +
      '<input class="tl-h" type="number" min="0" max="99" value="' + h + '"><span>h</span>' +
      '<input class="tl-m" type="number" min="0" max="59" value="' + m + '"><span>m</span>' +
      '<input class="tl-s" type="number" min="0" max="59" value="' + s + '"><span>s</span>' +
      '<button type="button" class="tl-del" title="Remove">&#10005;</button>' +
      '</div></div>';
  }

  function currentValue(): any[] {
    const out: any[] = [];
    const rows = root.querySelectorAll('.tl-row');
    for (let i = 0; i < rows.length; i++) {
      const r = rows[i] as HTMLElement;
      const name = (r.querySelector('.tl-name') as HTMLInputElement).value;
      const h = parseInt((r.querySelector('.tl-h') as HTMLInputElement).value, 10) || 0;
      const m = parseInt((r.querySelector('.tl-m') as HTMLInputElement).value, 10) || 0;
      const s = parseInt((r.querySelector('.tl-s') as HTMLInputElement).value, 10) || 0;
      out.push({ name: name, seconds: clamp(h, 0, 99) * 3600 + clamp(m, 0, 59) * 60 + clamp(s, 0, 59) });
    }
    return out;
  }

  function updateAdd(): void {
    const rows = root.querySelectorAll('.tl-row');
    const add = root.querySelector('.tl-add') as HTMLButtonElement;
    if (add) { add.style.display = (rows.length >= MAX) ? 'none' : ''; }
  }

  function rebuild(list: any[]): void {
    const listEl = root.querySelector('.tl-list') as HTMLElement;
    let html = '';
    for (let i = 0; i < list.length && i < MAX; i++) {
      const e = list[i] || {};
      let sec = parseInt(e.seconds, 10); if (isNaN(sec) || sec < 0) { sec = 0; }
      const h = Math.floor(sec / 3600);
      const m = Math.floor((sec - h * 3600) / 60);
      const s = sec - h * 3600 - m * 60;
      html += rowHtml(typeof e.name === 'string' ? e.name : '', h, m, s);
    }
    listEl.innerHTML = html;
    updateAdd();
  }

  self._tlCurrentValue = currentValue;
  self._tlRebuild = rebuild;

  function rowIndexOf(node: Node | null): number {
    const rows = root.querySelectorAll('.tl-row');
    for (let i = 0; i < rows.length; i++) { if (rows[i] === node) { return i; } }
    return -1;
  }

  root.addEventListener('click', function(ev: Event) {
    let target = ev.target as HTMLElement;
    if (!target) { return; }
    if (target.tagName !== 'BUTTON') {
      target = target.closest ? (target.closest('button') as HTMLElement) : null as any;
    }
    if (!target) { return; }
    if (target.classList.contains('tl-add')) {
      const v = currentValue();
      if (v.length < MAX) { v.push({ name: '', seconds: 300 }); rebuild(v); self.trigger('change'); }
      return;
    }
    if (target.classList.contains('tl-del')) {
      const rowEl = target.parentNode ? (target.parentNode as HTMLElement).parentNode as HTMLElement : null;
      const idx = rowIndexOf(rowEl);
      if (idx === -1) { return; }
      const v = currentValue();
      v.splice(idx, 1);
      rebuild(v);
      self.trigger('change');
    }
  });
}

const timerListComponent = {
  name: 'timerList',
  template:
    '<div class="tl-rootc">' +
    '<div class="tl-list"></div>' +
    '<button type="button" class="tl-add">+ Add timer</button>' +
    '</div>',
  // Clay base theme forces button{min-width:12rem;margin:0 auto}; row controls must
  // override that and dark-theme the native inputs to match Clay's dark page.
  style:
    '.tl-row{margin:0 0 12px 0}' +
    '.tl-name{display:block;width:100%;box-sizing:border-box;height:2.8rem;margin:0 0 4px 0;' +
      'background-color:#767676;color:#fff;border:none;border-radius:0.3rem;padding:0 0.5rem;color-scheme:dark}' +
    '.tl-dur{display:flex;align-items:center}' +
    '.tl-dur input{flex:1 1 auto;min-width:0;width:3rem;height:2.8rem;margin:0 2px 0 0;text-align:right;' +
      'background-color:#767676;color:#fff;border:none;border-radius:0.3rem;padding:0 0.3rem;color-scheme:dark}' +
    '.tl-dur span{flex:0 0 auto;margin:0 6px 0 0;color:#fff}' +
    '.tl-dur button{flex:0 0 auto;min-width:0;width:2.8rem;height:2.8rem;margin:0 0 0 6px;padding:0}' +
    '.tl-add{margin:8px 0 10px 0}',
  manipulator: {
    get: function(this: any): any[] {
      return this._tlCurrentValue ? this._tlCurrentValue() : [];
    },
    set: function(this: any, value: any) {
      let list: any[] = [];
      let v: any = value;
      if (v && typeof v === 'object' && !Array.isArray(v) && v.value !== undefined) { v = v.value; }
      if (Array.isArray(v)) {
        list = v;
      } else if (typeof v === 'string' && v !== '') {
        try { const p = JSON.parse(v); if (Array.isArray(p)) { list = p; } } catch (e) { list = []; }
      }
      if (this._tlRebuild) { this._tlRebuild(list); }
      return this;
    },
  },
  defaults: { label: '' },
  initialize: timerListInitialize,
};

export = timerListComponent;
```

- [ ] **Step 2: Typecheck**

Run: `cd PebbleCountdownTimer && npm run typecheck`
Expected: no errors.

- [ ] **Step 3: Commit** (full verification of toSource-safety happens after the build in Task 4)

```bash
cd /home/dev/pebble-timetracking/PebbleCountdownTimer
git add src/ts/config_timer_list.ts
git commit -m "Add Clay timerList custom component"
cd /home/dev/pebble-timetracking && git add PebbleCountdownTimer && git commit -m "Bump PebbleCountdownTimer: timerList component"
```

---

## Task 4: Clay config + index wiring (phone → watch)

**Files:**
- Create: `PebbleCountdownTimer/src/ts/config_clay.ts`
- Create: `PebbleCountdownTimer/src/ts/index.ts`
- Delete: `PebbleCountdownTimer/src/ts/.keep`

- [ ] **Step 1: Write `src/ts/config_clay.ts`**

```ts
// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Tuomas Airaksinen

// Clay config for the Countdown timer app. The single `timerList` custom
// component (messageKey 'TimerList') is a Clay-only key (NOT in package.json
// messageKeys) — index.ts serializes it to the real CString key 'TimerConfig'
// on save, exactly as TimeStyle maps WidgetList -> SettingWidgetList.
const config = [
  { type: 'heading', defaultValue: 'Countdown timer' },
  {
    type: 'section',
    items: [
      { type: 'heading', defaultValue: 'Timers' },
      { type: 'text', defaultValue: 'Add timers below. On the watch, open a timer to Start/Pause/Reset it.' },
      { type: 'timerList', messageKey: 'TimerList', defaultValue: [{ name: 'Timer 1', seconds: 300 }] },
    ],
  },
  {
    type: 'section',
    items: [
      { type: 'heading', defaultValue: 'Display' },
      // radiogroup values MUST be strings (Clay gotcha); index.ts parseInts on save.
      { type: 'radiogroup', messageKey: 'SortOrder', label: 'Sort timers on watch by',
        defaultValue: '0', options: [
          { label: 'Most recently used', value: '0' },
          { label: 'Shortest remaining first', value: '1' },
          { label: 'Longest remaining first', value: '2' },
        ] },
    ],
  },
  { type: 'submit', defaultValue: 'Save' },
];

export = config;
```

- [ ] **Step 2: Write `src/ts/index.ts`**

```ts
// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Tuomas Airaksinen

import Clay from 'pebble-clay';
import clayConfig from './config_clay';
import timerListComponent from './config_timer_list';
import { timerListToString } from './timer_config';

const clay = new Clay(clayConfig, null, { autoHandleEvents: false });
clay.registerComponent(timerListComponent);

Pebble.addEventListener('showConfiguration', () => {
  Pebble.openURL(clay.generateUrl());
});

Pebble.addEventListener('webviewclosed', (e: any) => {
  if (!e || !e.response) { console.log('No settings changed'); return; }
  const raw = clay.getSettings(e.response, false);
  const s: Record<string, any> = {};
  Object.keys(raw).forEach((k) => {
    const v = raw[k];
    s[k] = (v && typeof v === 'object' && 'value' in v) ? v.value : v;
  });
  const dict: Record<string, any> = {};
  dict.TimerConfig = timerListToString(s.TimerList);
  dict.SortOrder = parseInt(s.SortOrder, 10) || 0;
  console.log('Sending TimerConfig: ' + JSON.stringify(dict.TimerConfig) + ' sort=' + dict.SortOrder);
  Pebble.sendAppMessage(dict, () => { console.log('config sent'); },
    () => { console.log('config send failed'); });
});
```

- [ ] **Step 3: Add the `Pebble`/`Clay` ambient types** so `tsc --strict` compiles. Check whether TimeStylePebble has `src/ts/types/` declarations:

Run: `ls ../TimeStylePebble/src/ts/types/`
Then copy what's needed:
```bash
cd PebbleCountdownTimer
mkdir -p src/ts/types
cp ../TimeStylePebble/src/ts/types/*.d.ts src/ts/types/ 2>/dev/null || true
```
If TimeStyle has no `pebble-clay`/`Pebble` global typings (it imports `Clay from 'pebble-clay'` and uses a global `Pebble`), add a minimal `src/ts/types/globals.d.ts`:
```ts
declare module 'pebble-clay';
declare const Pebble: any;
declare const window: any;
declare const document: any;
declare const console: any;
```
(Only include declarations not already provided by copied files or `lib: ["DOM"]` — `window`/`document`/`console` come from DOM lib, so likely only `declare module 'pebble-clay';` and `declare const Pebble: any;` are needed. Let `tsc` errors tell you which.)

- [ ] **Step 4: Remove the placeholder and typecheck**

```bash
cd PebbleCountdownTimer
rm -f src/ts/.keep
npm run typecheck
```
Expected: no errors.

- [ ] **Step 5: Clean build (messageKeys changed since Task 1 — they didn't, but a clean build regenerates pkjs and verifies bundling)**

```bash
cd PebbleCountdownTimer
PEBBLE_QEMU_PATH=~/.local/bin/qemu-pebble-headless pebble clean
PEBBLE_QEMU_PATH=~/.local/bin/qemu-pebble-headless pebble build
```
Expected: `'build' finished successfully`.

- [ ] **Step 6: Verify the serialized Clay pieces are helper-free** (the toSource-safety gate)

Run:
```bash
cd PebbleCountdownTimer
grep -nE '__spreadArray|__assign|__read|__values|_this' src/pkjs/config_timer_list.js src/pkjs/config_clay.js
```
Expected: **no output**. If anything matches, the component/config used spread/destructuring/a downlevel helper — fix the `.ts` and rebuild before committing.

- [ ] **Step 7: Commit**

```bash
cd /home/dev/pebble-timetracking/PebbleCountdownTimer
git add src/ts/config_clay.ts src/ts/index.ts src/ts/types
git commit -m "Wire Clay config + serialize timer list to TimerConfig on save"
cd /home/dev/pebble-timetracking && git add PebbleCountdownTimer && git commit -m "Bump PebbleCountdownTimer: Clay wiring"
```

---

## Task 5: C pure logic — `timer_calc.c/.h` + host gcc test

**Files:**
- Create: `PebbleCountdownTimer/src/c/timer_calc.h`
- Create: `PebbleCountdownTimer/src/c/timer_calc.c`
- Test: `PebbleCountdownTimer/tests/test_timer_calc.c`

`timer_calc.{c,h}` MUST NOT include `pebble.h` (only `<stdint.h>`, `<stddef.h>`, `<string.h>`, `<stdbool.h>`, `<time.h>`). It holds all pure logic so it compiles + runs on the host with gcc, mirroring `TimeStylePebble/tests/test_twt_calc.c`. The watch `main.c` and the test both compile these files.

- [ ] **Step 1: Write `src/c/timer_calc.h`**

```c
// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Tuomas Airaksinen
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define MAX_TIMERS 16    // matches MAX_TIMERS in src/ts/timer_config.ts
#define NAME_LEN   31    // matches NAME_MAX in src/ts/timer_config.ts

typedef enum { TS_IDLE = 0, TS_RUNNING = 1, TS_PAUSED = 2, TS_DONE = 3 } TimerState;

// List sort modes (matches SortOrder values in src/ts/config_clay.ts).
typedef enum { SORT_MRU = 0, SORT_SHORTEST = 1, SORT_LONGEST = 2 } SortMode;

typedef struct {
  char name[NAME_LEN + 1];
  int32_t duration;     // configured length, seconds (>=1)
  TimerState state;
  int64_t end_time;     // epoch secs; valid when RUNNING
  int32_t remaining;    // secs left; valid when IDLE/PAUSED/DONE
  int64_t last_used;    // epoch secs of last user action; drives list order
} Timer;

// Parse the TimerConfig string into config-only timers (state IDLE, remaining=
// duration, end_time=0, last_used=0). Returns count (<= MAX_TIMERS).
int tc_parse_config(const char *buf, Timer *out, int max);

// "M:SS" when < 1h, else "H:MM:SS". Writes into buf (size n).
void tc_format_remaining(char *buf, size_t n, int32_t secs);

// Seconds left to show for a timer at time `now`.
int32_t tc_remaining_now(const Timer *t, int64_t now);

// Soonest end_time among RUNNING timers. Returns true + *out set, or false.
bool tc_soonest_end(const Timer *t, int count, int64_t *out);

// Fill order[count] with timer indices sorted per `mode` (SORT_MRU: last_used
// desc; SORT_SHORTEST/LONGEST: remaining-at-`now` asc/desc), ties by index asc.
void tc_display_order(const Timer *t, int count, SortMode mode, int64_t now, int *order);

// State transitions (each stamps last_used = now where it represents a user action).
void tc_start(Timer *t, int64_t now);
void tc_pause(Timer *t, int64_t now);
void tc_reset(Timer *t, int64_t now);

// If RUNNING and end_time <= now: mark DONE, remaining 0, return true. Else false.
bool tc_check_expiry(Timer *t, int64_t now);

// Merge a freshly parsed config (cfg/cfgN) over current runtime state (cur/curN)
// by list position into out (size MAX_TIMERS); returns new count. Unchanged rows
// keep their state; duration-changed rows keep state with remaining re-derived
// for non-RUNNING; new rows start IDLE; dropped rows disappear.
int tc_reconcile(const Timer *cur, int curN, const Timer *cfg, int cfgN, Timer *out);
```

- [ ] **Step 2: Write the failing test `tests/test_timer_calc.c`**

```c
// SPDX-License-Identifier: GPL-3.0-only
#include "timer_calc.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void) {
  Timer t[MAX_TIMERS];

  // --- tc_parse_config ---
  int n = tc_parse_config("Egg\x1f""300\x1e""Tea\x1f""120", t, MAX_TIMERS);
  assert(n == 2);
  assert(strcmp(t[0].name, "Egg") == 0 && t[0].duration == 300);
  assert(t[0].state == TS_IDLE && t[0].remaining == 300);
  assert(strcmp(t[1].name, "Tea") == 0 && t[1].duration == 120);
  assert(tc_parse_config("", t, MAX_TIMERS) == 0);
  // skip zero/invalid durations; cap at MAX_TIMERS
  assert(tc_parse_config("a\x1f""0\x1e""b\x1f""60", t, MAX_TIMERS) == 1);
  assert(strcmp(t[0].name, "b") == 0);

  // --- tc_format_remaining ---
  char b[16];
  tc_format_remaining(b, sizeof(b), 0);     assert(strcmp(b, "0:00") == 0);
  tc_format_remaining(b, sizeof(b), 65);    assert(strcmp(b, "1:05") == 0);
  tc_format_remaining(b, sizeof(b), 599);   assert(strcmp(b, "9:59") == 0);
  tc_format_remaining(b, sizeof(b), 3600);  assert(strcmp(b, "1:00:00") == 0);
  tc_format_remaining(b, sizeof(b), 3725);  assert(strcmp(b, "1:02:05") == 0);

  // --- transitions + remaining_now ---
  Timer x; memset(&x, 0, sizeof(x)); x.duration = 300; x.remaining = 300; x.state = TS_IDLE;
  tc_start(&x, 1000);
  assert(x.state == TS_RUNNING && x.end_time == 1300 && x.last_used == 1000);
  assert(tc_remaining_now(&x, 1000) == 300);
  assert(tc_remaining_now(&x, 1290) == 10);
  assert(tc_remaining_now(&x, 1400) == 0);   // clamps at 0
  tc_pause(&x, 1290);
  assert(x.state == TS_PAUSED && x.remaining == 10 && x.last_used == 1290);
  tc_start(&x, 2000);
  assert(x.end_time == 2010);                 // resumes from remaining
  tc_reset(&x, 2500);
  assert(x.state == TS_IDLE && x.remaining == 300 && x.last_used == 2500);

  // --- expiry ---
  Timer y; memset(&y, 0, sizeof(y)); y.duration = 60; y.state = TS_RUNNING; y.end_time = 100;
  assert(tc_check_expiry(&y, 50) == false);
  assert(tc_check_expiry(&y, 100) == true && y.state == TS_DONE && y.remaining == 0);
  assert(tc_check_expiry(&y, 200) == false); // already DONE, not re-fired

  // --- soonest_end ---
  Timer s[3]; memset(s, 0, sizeof(s));
  s[0].state = TS_RUNNING; s[0].end_time = 500;
  s[1].state = TS_PAUSED;  s[1].end_time = 0;
  s[2].state = TS_RUNNING; s[2].end_time = 300;
  int64_t soon;
  assert(tc_soonest_end(s, 3, &soon) == true && soon == 300);
  Timer none[1]; memset(none, 0, sizeof(none)); none[0].state = TS_IDLE;
  assert(tc_soonest_end(none, 1, &soon) == false);

  // --- display_order: SORT_MRU (most-recently-used first, ties by index) ---
  Timer d[3]; memset(d, 0, sizeof(d));
  d[0].last_used = 10; d[1].last_used = 0; d[2].last_used = 30;
  int order[3];
  tc_display_order(d, 3, SORT_MRU, 0, order);
  assert(order[0] == 2 && order[1] == 0 && order[2] == 1);

  // --- display_order: SORT_SHORTEST / SORT_LONGEST by remaining-at-now ---
  Timer e[3]; memset(e, 0, sizeof(e));
  e[0].state = TS_RUNNING; e[0].end_time = 1300;  // 300s left at now=1000
  e[1].state = TS_PAUSED;  e[1].remaining = 50;   // 50s
  e[2].state = TS_IDLE;    e[2].remaining = 120;  // 120s
  tc_display_order(e, 3, SORT_SHORTEST, 1000, order);
  assert(order[0] == 1 && order[1] == 2 && order[2] == 0);  // 50, 120, 300
  tc_display_order(e, 3, SORT_LONGEST, 1000, order);
  assert(order[0] == 0 && order[1] == 2 && order[2] == 1);  // 300, 120, 50

  // --- reconcile: unchanged row keeps RUNNING state; new row IDLE ---
  Timer cur[1]; memset(cur, 0, sizeof(cur));
  strcpy(cur[0].name, "Egg"); cur[0].duration = 300; cur[0].state = TS_RUNNING;
  cur[0].end_time = 9999; cur[0].last_used = 42;
  Timer cfg[2]; memset(cfg, 0, sizeof(cfg));
  strcpy(cfg[0].name, "Egg"); cfg[0].duration = 300; cfg[0].state = TS_IDLE; cfg[0].remaining = 300;
  strcpy(cfg[1].name, "New"); cfg[1].duration = 60; cfg[1].state = TS_IDLE; cfg[1].remaining = 60;
  Timer outr[MAX_TIMERS];
  int rn = tc_reconcile(cur, 1, cfg, 2, outr);
  assert(rn == 2);
  assert(outr[0].state == TS_RUNNING && outr[0].end_time == 9999 && outr[0].last_used == 42);
  assert(outr[1].state == TS_IDLE && outr[1].duration == 60 && strcmp(outr[1].name, "New") == 0);

  printf("All timer_calc tests passed\n");
  return 0;
}
```

- [ ] **Step 3: Run it, verify it fails**

Run: `cd PebbleCountdownTimer && gcc -I src/c tests/test_timer_calc.c src/c/timer_calc.c -o /tmp/tc_test`
Expected: FAIL — `timer_calc.c` doesn't exist / link errors.

- [ ] **Step 4: Write `src/c/timer_calc.c`**

```c
// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Tuomas Airaksinen
#include "timer_calc.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static void copy_name(char *dst, const char *src, size_t srclen) {
  size_t n = srclen < NAME_LEN ? srclen : NAME_LEN;
  memcpy(dst, src, n);
  dst[n] = '\0';
}

int tc_parse_config(const char *buf, Timer *out, int max) {
  int count = 0;
  if (!buf || buf[0] == '\0') { return 0; }
  const char *p = buf;
  while (*p && count < max) {
    const char *rec_end = strchr(p, '\x1e');
    const char *limit = rec_end ? rec_end : (p + strlen(p));
    const char *us = (const char *)memchr(p, '\x1f', (size_t)(limit - p));
    if (us) {
      int32_t seconds = (int32_t)strtol(us + 1, NULL, 10);
      if (seconds >= 1) {
        Timer *t = &out[count];
        memset(t, 0, sizeof(*t));
        copy_name(t->name, p, (size_t)(us - p));
        t->duration = seconds;
        t->state = TS_IDLE;
        t->remaining = seconds;
        count++;
      }
    }
    if (!rec_end) { break; }
    p = rec_end + 1;
  }
  return count;
}

void tc_format_remaining(char *buf, size_t n, int32_t secs) {
  if (secs < 0) { secs = 0; }
  int h = secs / 3600;
  int m = (secs % 3600) / 60;
  int s = secs % 60;
  if (h > 0) { snprintf(buf, n, "%d:%02d:%02d", h, m, s); }
  else { snprintf(buf, n, "%d:%02d", m, s); }
}

int32_t tc_remaining_now(const Timer *t, int64_t now) {
  if (t->state == TS_RUNNING) {
    int64_t r = t->end_time - now;
    if (r < 0) { r = 0; }
    return (int32_t)r;
  }
  return t->remaining;
}

bool tc_soonest_end(const Timer *t, int count, int64_t *out) {
  bool found = false;
  int64_t best = 0;
  for (int i = 0; i < count; i++) {
    if (t[i].state == TS_RUNNING) {
      if (!found || t[i].end_time < best) { best = t[i].end_time; found = true; }
    }
  }
  if (found && out) { *out = best; }
  return found;
}

// Comparison key for a timer under `mode`. Returns a 64-bit value; the sort puts
// HIGHER keys first, so we negate where the mode wants ascending order.
static int64_t order_key(const Timer *t, SortMode mode, int64_t now) {
  if (mode == SORT_SHORTEST) { return -(int64_t)tc_remaining_now(t, now); } // asc -> negate
  if (mode == SORT_LONGEST)  { return  (int64_t)tc_remaining_now(t, now); } // desc
  return t->last_used;                                                       // MRU: desc
}

void tc_display_order(const Timer *t, int count, SortMode mode, int64_t now, int *order) {
  for (int i = 0; i < count; i++) { order[i] = i; }
  // stable insertion sort: higher order_key first, ties keep ascending index
  for (int i = 1; i < count; i++) {
    int key = order[i];
    int64_t kv = order_key(&t[key], mode, now);
    int j = i - 1;
    while (j >= 0 && order_key(&t[order[j]], mode, now) < kv) {
      order[j + 1] = order[j];
      j--;
    }
    order[j + 1] = key;
  }
}

void tc_start(Timer *t, int64_t now) {
  // resume from the paused remainder, otherwise start a full duration
  int32_t rem = (t->state == TS_PAUSED) ? t->remaining : t->duration;
  if (rem < 1) { rem = t->duration; }
  t->end_time = now + rem;
  t->state = TS_RUNNING;
  t->last_used = now;
}

void tc_pause(Timer *t, int64_t now) {
  if (t->state == TS_RUNNING) {
    int64_t r = t->end_time - now;
    if (r < 0) { r = 0; }
    t->remaining = (int32_t)r;
  }
  t->state = TS_PAUSED;
  t->last_used = now;
}

void tc_reset(Timer *t, int64_t now) {
  t->state = TS_IDLE;
  t->remaining = t->duration;
  t->end_time = 0;
  t->last_used = now;
}

bool tc_check_expiry(Timer *t, int64_t now) {
  if (t->state == TS_RUNNING && t->end_time <= now) {
    t->state = TS_DONE;
    t->remaining = 0;
    return true;
  }
  return false;
}

int tc_reconcile(const Timer *cur, int curN, const Timer *cfg, int cfgN, Timer *out) {
  int n = cfgN > MAX_TIMERS ? MAX_TIMERS : cfgN;
  for (int i = 0; i < n; i++) {
    Timer t = cfg[i];   // start from config (IDLE, remaining=duration)
    if (i < curN && strcmp(cur[i].name, cfg[i].name) == 0 && cur[i].duration == cfg[i].duration) {
      // unchanged slot: keep all runtime state
      t = cur[i];
    } else if (i < curN) {
      // same position, changed name/duration: keep state but re-derive timing
      t.state = cur[i].state;
      t.last_used = cur[i].last_used;
      if (cur[i].state == TS_RUNNING) {
        t.end_time = cur[i].end_time;
      } else if (cur[i].state == TS_PAUSED) {
        t.remaining = cur[i].remaining > t.duration ? t.duration : cur[i].remaining;
      } else {
        t.state = TS_IDLE;
        t.remaining = t.duration;
      }
    }
    out[i] = t;
  }
  return n;
}
```

- [ ] **Step 5: Run the test, verify it passes**

Run: `cd PebbleCountdownTimer && gcc -I src/c tests/test_timer_calc.c src/c/timer_calc.c -o /tmp/tc_test && /tmp/tc_test`
Expected: `All timer_calc tests passed`.

- [ ] **Step 6: Confirm the app still builds** (timer_calc.c is now globbed into the app by `src/c/**/*.c`)

Run: `cd PebbleCountdownTimer && PEBBLE_QEMU_PATH=~/.local/bin/qemu-pebble-headless pebble build`
Expected: `'build' finished successfully` (main.c doesn't use it yet — that's fine).

- [ ] **Step 7: Commit**

```bash
cd /home/dev/pebble-timetracking/PebbleCountdownTimer
git add src/c/timer_calc.h src/c/timer_calc.c tests/test_timer_calc.c
git commit -m "Add timer_calc pure logic (C) + host gcc test"
cd /home/dev/pebble-timetracking && git add PebbleCountdownTimer && git commit -m "Bump PebbleCountdownTimer: timer_calc"
```

---

## Task 6: C persistence — `timer_store.c/.h`

**Files:**
- Create: `PebbleCountdownTimer/src/c/timer_store.h`
- Create: `PebbleCountdownTimer/src/c/timer_store.c`

Persists the timer array + wakeup id with a schema byte, using `persist_write_data`/`persist_read_data`. This is thin glue around `pebble.h`; not separately host-tested (verified via the emulator in Task 10).

- [ ] **Step 1: Write `src/c/timer_store.h`**

```c
// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Tuomas Airaksinen
#pragma once
#include "timer_calc.h"
#include <stdint.h>

#define PERSIST_KEY_SCHEMA    1
#define PERSIST_KEY_COUNT     2
#define PERSIST_KEY_WAKEUPID  3
#define PERSIST_KEY_SORTORDER 4
#define PERSIST_KEY_TIMER_BASE 100   // timer i -> key 100+i (one Timer per key; 256B/key cap)
#define STORE_SCHEMA 1

// Loads timers into out (capacity MAX_TIMERS); returns count, or 0 if none/old schema.
int store_load(Timer *out);
// Persists `count` timers (one per key) + schema + count.
void store_save(const Timer *t, int count);
// Wakeup id (-1 when none).
int32_t store_load_wakeup_id(void);
void store_save_wakeup_id(int32_t id);
// Sort mode (defaults to SORT_MRU=0 when unset).
int store_load_sort(void);
void store_save_sort(int mode);
```

- [ ] **Step 2: Write `src/c/timer_store.c`**

```c
// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Tuomas Airaksinen
#include <pebble.h>
#include "timer_store.h"
#include <string.h>

// One Timer per persist key (PERSIST_KEY_TIMER_BASE+i). A packed blob can't be
// used: persist_write_data caps at 256 B/key and 16 Timers (~1 KB) exceed that.
int store_load(Timer *out) {
  if (!persist_exists(PERSIST_KEY_SCHEMA) ||
      persist_read_int(PERSIST_KEY_SCHEMA) != STORE_SCHEMA) { return 0; }
  int count = persist_exists(PERSIST_KEY_COUNT) ? persist_read_int(PERSIST_KEY_COUNT) : 0;
  if (count > MAX_TIMERS) { count = MAX_TIMERS; }
  if (count < 0) { count = 0; }
  for (int i = 0; i < count; i++) {
    if (persist_exists(PERSIST_KEY_TIMER_BASE + i)) {
      persist_read_data(PERSIST_KEY_TIMER_BASE + i, &out[i], sizeof(Timer));
    } else {
      memset(&out[i], 0, sizeof(Timer));
    }
  }
  return count;
}

void store_save(const Timer *t, int count) {
  if (count > MAX_TIMERS) { count = MAX_TIMERS; }
  persist_write_int(PERSIST_KEY_SCHEMA, STORE_SCHEMA);
  persist_write_int(PERSIST_KEY_COUNT, count);
  for (int i = 0; i < count; i++) {
    persist_write_data(PERSIST_KEY_TIMER_BASE + i, &t[i], sizeof(Timer));
  }
  // drop any stale keys beyond the new count
  for (int i = count; i < MAX_TIMERS; i++) {
    if (persist_exists(PERSIST_KEY_TIMER_BASE + i)) { persist_delete(PERSIST_KEY_TIMER_BASE + i); }
  }
}

int32_t store_load_wakeup_id(void) {
  if (!persist_exists(PERSIST_KEY_WAKEUPID)) { return -1; }
  return persist_read_int(PERSIST_KEY_WAKEUPID);
}

void store_save_wakeup_id(int32_t id) {
  persist_write_int(PERSIST_KEY_WAKEUPID, id);
}

int store_load_sort(void) {
  if (!persist_exists(PERSIST_KEY_SORTORDER)) { return SORT_MRU; }
  return persist_read_int(PERSIST_KEY_SORTORDER);
}

void store_save_sort(int mode) {
  persist_write_int(PERSIST_KEY_SORTORDER, mode);
}
```

- [ ] **Step 3: Build**

Run: `cd PebbleCountdownTimer && PEBBLE_QEMU_PATH=~/.local/bin/qemu-pebble-headless pebble build`
Expected: `'build' finished successfully`.

- [ ] **Step 4: Commit**

```bash
cd /home/dev/pebble-timetracking/PebbleCountdownTimer
git add src/c/timer_store.h src/c/timer_store.c
git commit -m "Add timer_store persistence"
cd /home/dev/pebble-timetracking && git add PebbleCountdownTimer && git commit -m "Bump PebbleCountdownTimer: persistence"
```

---

## Task 7: Watch app — UI, wakeup, AppMessage (the integration task)

This task assembles `main.c` into the full app, in small build-and-screenshot increments. It uses `timer_calc`, `timer_store`, and the SDK Wakeup/ActionMenu/MenuLayer/vibes APIs. Boot the emulator ONCE before starting:

```bash
cd /home/dev/pebble-timetracking
scripts/pebble-emu-boot.sh emery
```
Then reuse it for every install/screenshot (do NOT `pebble kill` between steps).

**Files:**
- Modify: `PebbleCountdownTimer/src/c/main.c` (replace the stub from Task 1 entirely)

- [ ] **Step 1: Write the full `src/c/main.c`**

```c
// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Tuomas Airaksinen
#include <pebble.h>
#include "timer_calc.h"
#include "timer_store.h"

static Window *s_window;
static MenuLayer *s_menu;
static AppTimer *s_tick;

static Timer s_timers[MAX_TIMERS];
static int s_count = 0;
static int s_order[MAX_TIMERS];   // display order, rebuilt on reload per s_sort
static SortMode s_sort = SORT_MRU;

static int64_t now_s(void) { return (int64_t)time(NULL); }

// ---- wakeup: keep exactly ONE armed for the soonest running end_time ----
static void rearm_wakeup(void) {
  int32_t old = store_load_wakeup_id();
  if (old >= 0) { wakeup_cancel(old); store_save_wakeup_id(-1); }
  int64_t soon;
  if (tc_soonest_end(s_timers, s_count, &soon)) {
    time_t when = (time_t)soon;
    time_t nowt = time(NULL);
    if (when <= nowt) { when = nowt + 1; }
    // wakeup needs >=1 min separation from other apps; nudge forward on failure.
    for (int attempt = 0; attempt < 4; attempt++) {
      WakeupId id = wakeup_schedule(when, 0, true);
      if (id >= 0) { store_save_wakeup_id(id); return; }
      when += 60;   // E_RANGE / slot taken: try the next minute
    }
    APP_LOG(APP_LOG_LEVEL_WARNING, "wakeup_schedule failed after retries");
  }
}

static void persist_all(void) { store_save(s_timers, s_count); }

static void rebuild_order(void) { tc_display_order(s_timers, s_count, s_sort, now_s(), s_order); }

static void reload_ui(void) {
  rebuild_order();
  if (s_menu) { menu_layer_reload_data(s_menu); }
}

// Mark every expired RUNNING timer DONE; vibrate once if any fired. Returns true if any.
static bool sweep_expiries(bool vibrate) {
  bool any = false;
  int64_t now = now_s();
  for (int i = 0; i < s_count; i++) {
    if (tc_check_expiry(&s_timers[i], now)) { any = true; }
  }
  if (any && vibrate) { vibes_long_pulse(); }
  return any;
}

// ---- 1s foreground tick: refresh running rows + catch foreground expiries ----
static void tick_cb(void *ctx) {
  bool fired = sweep_expiries(true);
  // Any running timer still counting? keep ticking.
  bool running = false;
  for (int i = 0; i < s_count; i++) { if (s_timers[i].state == TS_RUNNING) { running = true; } }
  reload_ui();
  if (fired) { persist_all(); rearm_wakeup(); }
  s_tick = running ? app_timer_register(1000, tick_cb, NULL) : NULL;
}

static void ensure_ticking(void) {
  if (s_tick) { return; }
  for (int i = 0; i < s_count; i++) {
    if (s_timers[i].state == TS_RUNNING) { s_tick = app_timer_register(1000, tick_cb, NULL); return; }
  }
}

// ---- ActionMenu: Start/Pause + Reset for the selected timer ----
static ActionMenuLevel *s_action_root;

static void act_toggle(ActionMenu *am, const ActionMenuItem *item, void *ctx) {
  int idx = (int)(intptr_t)action_menu_get_context(am);
  Timer *t = &s_timers[idx];
  if (t->state == TS_RUNNING) { tc_pause(t, now_s()); }
  else { tc_start(t, now_s()); }
  persist_all(); rearm_wakeup(); ensure_ticking(); reload_ui();
}

static void act_reset(ActionMenu *am, const ActionMenuItem *item, void *ctx) {
  int idx = (int)(intptr_t)action_menu_get_context(am);
  tc_reset(&s_timers[idx], now_s());
  persist_all(); rearm_wakeup(); reload_ui();
}

// Free the level hierarchy after the menu closes (else it leaks per open).
static void action_menu_did_close(ActionMenu *am, const ActionMenuLevel *root, void *ctx) {
  action_menu_hierarchy_destroy(root, NULL, NULL);
  s_action_root = NULL;
}

static void open_action_menu(int timer_idx) {
  s_action_root = action_menu_level_create(2);
  Timer *t = &s_timers[timer_idx];
  action_menu_level_add_action(s_action_root,
    (t->state == TS_RUNNING) ? "Pause" : "Start", act_toggle, NULL);
  action_menu_level_add_action(s_action_root, "Reset", act_reset, NULL);
  ActionMenuConfig cfg = {
    .root_level = s_action_root,
    .context = (void *)(intptr_t)timer_idx,
    .colors = { .background = GColorChromeYellow, .foreground = GColorBlack },
    .did_close = action_menu_did_close,
    .align = ActionMenuAlignCenter,
  };
  action_menu_open(&cfg);
}

// ---- MenuLayer callbacks ----
static uint16_t ml_num_rows(MenuLayer *ml, uint16_t section, void *ctx) {
  return s_count == 0 ? 1 : s_count;
}
static int16_t ml_cell_height(MenuLayer *ml, MenuIndex *ci, void *ctx) { return 44; }

static const char *state_label(TimerState st) {
  switch (st) {
    case TS_RUNNING: return "Running";
    case TS_PAUSED:  return "Paused";
    case TS_DONE:    return "Done";
    default:         return "";
  }
}

static void ml_draw_row(GContext *gctx, const Layer *cell, MenuIndex *ci, void *ctx) {
  if (s_count == 0) {
    menu_cell_basic_draw(gctx, cell, "No timers", "Configure on your phone", NULL);
    return;
  }
  int idx = s_order[ci->row];
  Timer *t = &s_timers[idx];
  char rem[16]; tc_format_remaining(rem, sizeof(rem), tc_remaining_now(t, now_s()));
  char sub[40]; snprintf(sub, sizeof(sub), "%s  %s", rem, state_label(t->state));
  menu_cell_basic_draw(gctx, cell, t->name[0] ? t->name : "(unnamed)", sub, NULL);
}

static void ml_select(MenuLayer *ml, MenuIndex *ci, void *ctx) {
  if (s_count == 0) { return; }
  open_action_menu(s_order[ci->row]);
}

// ---- AppMessage inbox: a TimerConfig string -> reconcile ----
static void inbox_received(DictionaryIterator *iter, void *ctx) {
  Tuple *sort = dict_find(iter, MESSAGE_KEY_SortOrder);
  if (sort) {
    int m = (int)sort->value->int32;
    if (m < SORT_MRU || m > SORT_LONGEST) { m = SORT_MRU; }
    s_sort = (SortMode)m;
    store_save_sort(s_sort);
  }
  Tuple *cfg = dict_find(iter, MESSAGE_KEY_TimerConfig);
  if (cfg) {
    Timer parsed[MAX_TIMERS];
    int pn = tc_parse_config(cfg->value->cstring, parsed, MAX_TIMERS);
    Timer merged[MAX_TIMERS];
    int mn = tc_reconcile(s_timers, s_count, parsed, pn, merged);
    memcpy(s_timers, merged, sizeof(Timer) * (size_t)mn);
    s_count = mn;
    sweep_expiries(false);
    persist_all(); rearm_wakeup(); ensure_ticking();
  }
  reload_ui();
}

// ---- window ----
static void window_load(Window *w) {
  Layer *root = window_get_root_layer(w);
  GRect bounds = layer_get_bounds(root);
  s_menu = menu_layer_create(bounds);
  menu_layer_set_callbacks(s_menu, NULL, (MenuLayerCallbacks){
    .get_num_rows = ml_num_rows,
    .get_cell_height = ml_cell_height,
    .draw_row = ml_draw_row,
    .select_click = ml_select,
  });
  menu_layer_set_click_config_onto_window(s_menu, w);
  layer_add_child(root, menu_layer_get_layer(s_menu));
}
static void window_unload(Window *w) { menu_layer_destroy(s_menu); s_menu = NULL; }

static void init(void) {
  s_count = store_load(s_timers);
  s_sort = (SortMode)store_load_sort();
  // If launched by a wakeup, the firing event was already consumed; sweep now.
  WakeupId wid; int32_t cookie;
  bool by_wakeup = wakeup_get_launch_event(&wid, &cookie);
  if (by_wakeup) { store_save_wakeup_id(-1); }
  bool fired = sweep_expiries(by_wakeup);   // vibrate only on wakeup launch
  if (fired) { persist_all(); }
  rearm_wakeup();

  app_message_register_inbox_received(inbox_received);
  app_message_open(app_message_inbox_size_maximum(), app_message_outbox_size_maximum());

  s_window = window_create();
  window_set_window_handlers(s_window, (WindowHandlers){ .load = window_load, .unload = window_unload });
  window_stack_push(s_window, true);
  rebuild_order();
  ensure_ticking();
}

static void deinit(void) {
  if (s_tick) { app_timer_cancel(s_tick); }
  persist_all();
  rearm_wakeup();   // ensure the closed-app wakeup reflects final state
  window_destroy(s_window);
}

int main(void) { init(); app_event_loop(); deinit(); }
```

- [ ] **Step 2: Build**

Run: `cd PebbleCountdownTimer && PEBBLE_QEMU_PATH=~/.local/bin/qemu-pebble-headless pebble build`
Expected: `'build' finished successfully`. If the C side errors with `'MESSAGE_KEY_TimerConfig' undeclared`, run `pebble clean && pebble build` (cached message-key macros).

- [ ] **Step 3: Install on the running emery emulator and screenshot the empty state**

```bash
cd PebbleCountdownTimer
PEBBLE_QEMU_PATH=~/.local/bin/qemu-pebble-headless pebble install --emulator emery
PEBBLE_QEMU_PATH=~/.local/bin/qemu-pebble-headless pebble screenshot --no-open /tmp/cdt_empty.png
```
Expected: shows the "No timers / Configure on your phone" row. **Surface `/tmp/cdt_empty.png` to the user** (SendUserFile).

- [ ] **Step 4: Seed demo timers to screenshot the populated list + states**

Temporarily edit `init()` to seed `s_timers` when `s_count == 0` (mirrors the superrepo CLAUDE.md "seed in the else branch" trick). Add right after `s_count = store_load(s_timers);`:
```c
#ifdef SCREENSHOT_FIXTURES
  if (s_count == 0) {
    s_count = 3;
    memset(s_timers, 0, sizeof(s_timers));
    strcpy(s_timers[0].name, "Egg"); s_timers[0].duration = 300; s_timers[0].state = TS_RUNNING; s_timers[0].end_time = time(NULL) + 184; s_timers[0].last_used = time(NULL);
    strcpy(s_timers[1].name, "Tea"); s_timers[1].duration = 120; s_timers[1].state = TS_PAUSED; s_timers[1].remaining = 75; s_timers[1].last_used = time(NULL) - 10;
    strcpy(s_timers[2].name, "Laundry"); s_timers[2].duration = 3600; s_timers[2].state = TS_DONE; s_timers[2].remaining = 0; s_timers[2].last_used = 0;
  }
#endif
```
Then:
```bash
SCREENSHOT_FIXTURES=1 PEBBLE_QEMU_PATH=~/.local/bin/qemu-pebble-headless pebble build
PEBBLE_QEMU_PATH=~/.local/bin/qemu-pebble-headless pebble install --emulator emery
PEBBLE_QEMU_PATH=~/.local/bin/qemu-pebble-headless pebble screenshot --no-open /tmp/cdt_list.png
```
Expected: three rows, Egg on top (most-recently-used), each with `M:SS Running/Paused/Done`. **Surface `/tmp/cdt_list.png`.** Also screenshot the ActionMenu: press Select via `pebble emu-app-config`? — not available; instead verify the ActionMenu opens by a separate manual note (the action menu requires a button press the headless emu can't inject reliably). Document that ActionMenu is verified on hardware in Task 8.

- [ ] **Step 5: Build the normal (non-fixture) bundle for committing** and confirm it still builds clean:

```bash
PEBBLE_QEMU_PATH=~/.local/bin/qemu-pebble-headless pebble build
```
(The `#ifdef SCREENSHOT_FIXTURES` block stays in — it's compiled out of the normal build, same as TimeStyle.)

- [ ] **Step 6: Commit**

```bash
cd /home/dev/pebble-timetracking/PebbleCountdownTimer
git add src/c/main.c
git commit -m "Implement watch UI, single-wakeup background alerting, AppMessage config"
cd /home/dev/pebble-timetracking && git add PebbleCountdownTimer && git commit -m "Bump PebbleCountdownTimer: watch app"
```

---

## Task 8: End-to-end verification on the real watch + appstore metadata

**Files:**
- Create: `PebbleCountdownTimer/appstore/{title,description,category,support_email,source_url}.txt` (model on `../PebbleTrackWorkTime/appstore/`)

- [ ] **Step 1: Sideload to the real Pebble Time 2 via CloudPebble** (per superrepo CLAUDE.md). Ensure `pebble login --status` is OK, then:
```bash
cd PebbleCountdownTimer
PEBBLE_QEMU_PATH=~/.local/bin/qemu-pebble-headless pebble build
pebble install --cloudpebble build/countdown-timer.pbw
```
(Needs `gie net loose`/allowlisted relay hosts as documented.)

- [ ] **Step 2: Manually verify the end-to-end flow on hardware** and report results to the user:
  - Open the app's config on the phone (Core app → app settings): the `timerList` page renders with the dark-themed rows, Add/✕ work, max 16 enforced; the SortOrder radiogroup shows three options.
  - Add 2-3 timers with names + H/M/S, Save. The watch list shows them.
  - Open a timer → ActionMenu shows Start (then Pause)/Reset; Start begins counting; the row floats to the top (MRU).
  - Start two timers; close the app to the watchface; confirm BOTH vibrate at their times (single-wakeup re-arm), and reopening shows them Done.
  - Reset returns a timer to its full duration.

- [ ] **Step 3: Write appstore metadata** — `title.txt` ("Countdown timer"), `description.txt` (short blurb), `category.txt` (match PTWT's category), `support_email.txt` (copy from PTWT), `source_url.txt` (the PebbleCountdownTimer repo URL).

- [ ] **Step 4: Run all tests one final time**

```bash
cd PebbleCountdownTimer
npm test
gcc -I src/c tests/test_timer_calc.c src/c/timer_calc.c -o /tmp/tc_test && /tmp/tc_test
PEBBLE_QEMU_PATH=~/.local/bin/qemu-pebble-headless pebble build
```
Expected: TS tests pass, `All timer_calc tests passed`, build succeeds.

- [ ] **Step 5: Update `README.md`** — replace the placeholder text with a short description of the app (what it does, that config is phone-side Clay, build commands).

- [ ] **Step 6: Commit**

```bash
cd /home/dev/pebble-timetracking/PebbleCountdownTimer
git add appstore README.md
git commit -m "Add appstore metadata + README"
cd /home/dev/pebble-timetracking && git add PebbleCountdownTimer && git commit -m "Bump PebbleCountdownTimer: appstore metadata + README"
```

---

## Notes for the implementer

- **`intptr_t`** needs `#include <stdint.h>` (pebble.h pulls it in; if not, add it to main.c). Passing the timer index through the ActionMenu `context` avoids a global "selected index".
- **ActionMenu callbacks fire after the menu closes** — they mutate `s_timers`, persist, re-arm the wakeup, and `reload_ui()`; that's safe because the MenuLayer is still alive underneath.
- **Wakeup edge**: when the watch was off at the scheduled minute, `notify_if_missed=true` makes it fire late; `sweep_expiries` on the next launch handles already-passed timers regardless.
- **Do not add keys to `messageKeys`** beyond `TimerConfig` without re-reading the superrepo CLAUDE.md note and running `pebble clean`.
- If `menu_cell_basic_draw`'s default row height differs per platform, the fixed 44px in `ml_cell_height` is fine for emery/diorite; adjust only if rows clip.
```
