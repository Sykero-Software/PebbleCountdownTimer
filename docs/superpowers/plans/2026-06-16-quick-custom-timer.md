# Quick custom timer Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let the user create, adjust and start an unnamed one-off countdown timer on the watch (long SELECT → detail window → "Save as new & start"), persisting it to the phone config without losing it across reconciles.

**Architecture:** Reuse the existing per-timer detail window, reached by a new list long-SELECT. Idle/done duration tuning adjusts `remaining` (not the template) and `tc_start` honors it. "Save as new" creates a local `custom`-flagged running timer and sends `AddTimer` to the phone; `tc_reconcile` preserves trailing `custom` rows until the phone config absorbs them. The phone appends the timer to `timer_config` + the Clay store.

**Tech Stack:** C (Pebble SDK, host-tested via gcc), TypeScript→JS PKJS (node --test), Clay config.

**Spec:** `docs/superpowers/specs/2026-06-16-quick-custom-timer-design.md`

**Test commands:**
- C core: `gcc -I src/c tests/test_timer_calc.c src/c/timer_calc.c -o /tmp/tct && /tmp/tct`
- Phone: `npm test`
- Watch build (after a `messageKeys` change): `pebble clean && pebble build`

---

### Task 1: Add `Timer.custom` field + bump persistence schema

**Files:**
- Modify: `src/c/timer_calc.h` (the `Timer` struct)
- Modify: `src/c/timer_store.h:11` (`STORE_SCHEMA`)
- Test: `tests/test_timer_calc.c`

- [ ] **Step 1: Add a regression assertion that the field defaults false on parse**

In `tests/test_timer_calc.c`, just after the existing `tc_parse_config` block (after the `assert(strcmp(t[0].name, "b") == 0);` line), add:

```c
  // parsed timers are config-backed, never custom
  assert(t[0].custom == false);
```

- [ ] **Step 2: Run the C test to verify it fails to compile**

Run: `gcc -I src/c tests/test_timer_calc.c src/c/timer_calc.c -o /tmp/tct && /tmp/tct`
Expected: FAIL — `error: 'Timer' has no member named 'custom'`.

- [ ] **Step 3: Add the field**

In `src/c/timer_calc.h`, add to the `Timer` struct (after `last_used`):

```c
  bool custom;          // true = created on the watch; preserved across config reconcile until absorbed
```

- [ ] **Step 4: Bump the persistence schema**

In `src/c/timer_store.h`, change:

```c
#define STORE_SCHEMA 2
```

(was `1`; the wider `Timer` invalidates old persisted blobs — `store_load` returns 0 for a mismatched schema and the watch re-fetches config from the phone, a one-time reset.)

- [ ] **Step 5: Run the C test to verify it passes**

Run: `gcc -I src/c tests/test_timer_calc.c src/c/timer_calc.c -o /tmp/tct && /tmp/tct`
Expected: PASS — `All timer_calc tests passed`.

- [ ] **Step 6: Commit**

```bash
git add src/c/timer_calc.h src/c/timer_store.h tests/test_timer_calc.c
git commit -m "Timer.custom flag + STORE_SCHEMA 2"
```

---

### Task 2: `tc_start` starts a non-running timer from `remaining`

**Files:**
- Modify: `src/c/timer_calc.c:108-115` (`tc_start`)
- Test: `tests/test_timer_calc.c`

- [ ] **Step 1: Write the failing tests**

In `tests/test_timer_calc.c`, just before the `printf("All timer_calc tests passed\n");` line, add:

```c
  // --- tc_start: non-running timers start from `remaining` (one-off adjust) ---
  // idle, remaining bumped above duration -> starts from remaining
  Timer si; memset(&si, 0, sizeof(si)); si.duration = 300; si.state = TS_IDLE; si.remaining = 420;
  tc_start(&si, 1000);
  assert(si.state == TS_RUNNING && si.end_time == 1420);
  // idle, remaining == duration (normal) -> unchanged behavior
  Timer sn; memset(&sn, 0, sizeof(sn)); sn.duration = 300; sn.state = TS_IDLE; sn.remaining = 300;
  tc_start(&sn, 1000);
  assert(sn.end_time == 1300);
  // done, remaining 0 -> falls back to duration
  Timer sd; memset(&sd, 0, sizeof(sd)); sd.duration = 90; sd.state = TS_DONE; sd.remaining = 0;
  tc_start(&sd, 1000);
  assert(sd.end_time == 1090);
  // done, remaining adjusted up -> starts from remaining
  Timer sda; memset(&sda, 0, sizeof(sda)); sda.duration = 90; sda.state = TS_DONE; sda.remaining = 150;
  tc_start(&sda, 1000);
  assert(sda.end_time == 1150);
```

- [ ] **Step 2: Run the C test to verify it fails**

Run: `gcc -I src/c tests/test_timer_calc.c src/c/timer_calc.c -o /tmp/tct && /tmp/tct`
Expected: FAIL — assertion `si.end_time == 1420` (idle currently starts from `duration`, giving 1300).

- [ ] **Step 3: Implement the change**

In `src/c/timer_calc.c`, replace the body of `tc_start` (lines 108-115):

```c
void tc_start(Timer *t, int64_t now) {
  // Non-running timers start from `remaining` (PAUSED resume, or an IDLE/DONE
  // timer whose duration was tuned with +/- before starting). Fall back to the
  // full duration when remaining is unset/zero, so a plain Start is unchanged.
  int32_t rem = (t->state == TS_RUNNING) ? t->duration : t->remaining;
  if (rem < 1) { rem = t->duration; }
  t->end_time = now + rem;
  t->state = TS_RUNNING;
  t->last_used = now;
}
```

- [ ] **Step 4: Run the C test to verify it passes**

Run: `gcc -I src/c tests/test_timer_calc.c src/c/timer_calc.c -o /tmp/tct && /tmp/tct`
Expected: PASS — `All timer_calc tests passed` (the existing pause→resume test `x.end_time == 2010` still holds, since PAUSED already used `remaining`).

- [ ] **Step 5: Commit**

```bash
git add src/c/timer_calc.c tests/test_timer_calc.c
git commit -m "tc_start: non-running timers start from remaining"
```

---

### Task 3: `tc_reconcile` preserves trailing `custom` rows

**Files:**
- Modify: `src/c/timer_calc.c` (`tc_reconcile`)
- Test: `tests/test_timer_calc.c`

- [ ] **Step 1: Write the failing tests**

In `tests/test_timer_calc.c`, immediately after the existing reconcile block (after `assert(outr[1].state == TS_IDLE && outr[1].duration == 60 && strcmp(outr[1].name, "New") == 0);`), add:

```c
  // a trailing custom row (cur beyond cfgN) is preserved, appended after config
  Timer cur2[2]; memset(cur2, 0, sizeof(cur2));
  strcpy(cur2[0].name, "Egg"); cur2[0].duration = 300; cur2[0].state = TS_IDLE; cur2[0].remaining = 300;
  cur2[1].duration = 600; cur2[1].state = TS_RUNNING; cur2[1].end_time = 7777; cur2[1].custom = true;
  Timer cfg2[1]; memset(cfg2, 0, sizeof(cfg2));
  strcpy(cfg2[0].name, "Egg"); cfg2[0].duration = 300; cfg2[0].state = TS_IDLE; cfg2[0].remaining = 300;
  Timer outr2[MAX_TIMERS];
  int rn2 = tc_reconcile(cur2, 2, cfg2, 1, outr2);
  assert(rn2 == 2);
  assert(outr2[1].state == TS_RUNNING && outr2[1].end_time == 7777 && outr2[1].custom == true);

  // a trailing NON-custom row is still dropped (classic behavior)
  Timer cur3[2]; memset(cur3, 0, sizeof(cur3));
  strcpy(cur3[0].name, "Egg"); cur3[0].duration = 300;
  cur3[1].duration = 600; cur3[1].state = TS_RUNNING; cur3[1].custom = false;
  int rn3 = tc_reconcile(cur3, 2, cfg2, 1, outr2);
  assert(rn3 == 1);

  // once the config grows to include the custom row's position, the flag clears (absorbed)
  Timer cfg4[2]; memset(cfg4, 0, sizeof(cfg4));
  strcpy(cfg4[0].name, "Egg"); cfg4[0].duration = 300; cfg4[0].remaining = 300;
  cfg4[1].duration = 600; cfg4[1].remaining = 600;   // unnamed, matches the custom row's duration
  int rn4 = tc_reconcile(cur2, 2, cfg4, 2, outr2);
  assert(rn4 == 2);
  assert(outr2[1].state == TS_RUNNING && outr2[1].end_time == 7777 && outr2[1].custom == false);
```

- [ ] **Step 2: Run the C test to verify it fails**

Run: `gcc -I src/c tests/test_timer_calc.c src/c/timer_calc.c -o /tmp/tct && /tmp/tct`
Expected: FAIL — `rn2 == 2` (current `tc_reconcile` returns `cfgN`=1 and never preserves trailing rows).

- [ ] **Step 3: Implement the change**

In `src/c/timer_calc.c`, replace the whole `tc_reconcile` function with:

```c
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
    t.custom = false;   // a config-backed position is no longer watch-local (absorbs custom rows)
    out[i] = t;
  }
  // Preserve watch-created (custom) rows that sit beyond the config length, so an
  // on-watch "Save as new" timer is not dropped before the phone config grows to
  // include it. Non-custom trailing rows are dropped as before.
  for (int i = cfgN; i < curN && n < MAX_TIMERS; i++) {
    if (cur[i].custom) { out[n] = cur[i]; n++; }
  }
  return n;
}
```

- [ ] **Step 4: Run the C test to verify it passes**

Run: `gcc -I src/c tests/test_timer_calc.c src/c/timer_calc.c -o /tmp/tct && /tmp/tct`
Expected: PASS — `All timer_calc tests passed`.

- [ ] **Step 5: Commit**

```bash
git add src/c/timer_calc.c tests/test_timer_calc.c
git commit -m "tc_reconcile: preserve trailing custom rows until config absorbs them"
```

---

### Task 4: Append the `AddTimer` message key

**Files:**
- Modify: `package.json` (`pebble.messageKeys`)

- [ ] **Step 1: Append the key**

In `package.json`, change the `messageKeys` array (keep order; append only):

```json
"messageKeys": ["TimerConfig", "SortOrder", "Request", "AutoReturn", "RunningFirst", "AddTimer"]
```

- [ ] **Step 2: Regenerate the C macro**

Run: `pebble clean && pebble build`
Expected: build succeeds; `MESSAGE_KEY_AddTimer` is now defined (the C code that uses it lands in Task 7 — until then it is just declared, which is fine).

- [ ] **Step 3: Commit**

```bash
git add package.json
git commit -m "messageKeys: append AddTimer"
```

---

### Task 5: Phone-side `appendCustomTimer` helper (TDD)

**Files:**
- Create: `src/ts/add_timer.ts`
- Test: `tests/add_timer.test.js`

- [ ] **Step 1: Write the failing test**

Create `tests/add_timer.test.js`:

```js
// SPDX-License-Identifier: GPL-3.0-only
const test = require('node:test');
const assert = require('node:assert');
const { appendCustomTimer } = require('../src/pkjs/add_timer');

function fakeStore(init) {
  const m = new Map(Object.entries(init || {}));
  return {
    get: (k) => (m.has(k) ? m.get(k) : null),
    set: (k, v) => m.set(k, v),
    dump: () => m,
  };
}
const RS = '\x1e', US = '\x1f';

test('appends an unnamed timer to timer_config', () => {
  const s = fakeStore({ timer_config: 'Egg' + US + '300' });
  const str = appendCustomTimer(s.get, s.set, 120);
  assert.strictEqual(str, 'Egg' + US + '300' + RS + US + '120');
  assert.strictEqual(s.get('timer_config'), str);
});

test('mirrors the new timer into the clay-settings TimerList', () => {
  const s = fakeStore({ timer_config: '', 'clay-settings': JSON.stringify({ SortOrder: '0' }) });
  appendCustomTimer(s.get, s.set, 90);
  const cs = JSON.parse(s.get('clay-settings'));
  assert.deepStrictEqual(cs.TimerList, [{ name: '', seconds: 90 }]);
  assert.strictEqual(cs.SortOrder, '0'); // other keys preserved
});

test('rejects invalid or full', () => {
  const s = fakeStore({ timer_config: '' });
  assert.strictEqual(appendCustomTimer(s.get, s.set, 0), null);
  assert.strictEqual(appendCustomTimer(s.get, s.set, -5), null);
  // fill to MAX_TIMERS (16)
  const many = [];
  for (let i = 0; i < 16; i++) { many.push('t' + US + '60'); }
  const s2 = fakeStore({ timer_config: many.join(RS) });
  assert.strictEqual(appendCustomTimer(s2.get, s2.set, 60), null);
});
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `npm test`
Expected: FAIL — `Cannot find module '../src/pkjs/add_timer'` (the `pretest` `tsc` has not emitted it yet).

- [ ] **Step 3: Implement the helper**

Create `src/ts/add_timer.ts`:

```ts
// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Tuomas Airaksinen

// Append an unnamed, watch-created timer to the phone's persisted config and to
// the Clay config-page store, so it survives and shows up on the config page.
// `get`/`set` read/write localStorage by key. Returns the new TimerConfig string,
// or null when `seconds` is invalid or the list is already at MAX_TIMERS.
import { stringToTimerList, timerListToString, MAX_TIMERS } from './timer_config';

export function appendCustomTimer(
  get: (k: string) => string | null,
  set: (k: string, v: string) => void,
  seconds: any
): string | null {
  const secs = Math.floor(Number(seconds));
  if (!(secs >= 1)) { return null; }
  const list = stringToTimerList(get('timer_config') || '');
  if (list.length >= MAX_TIMERS) { return null; }
  list.push({ name: '', seconds: secs });
  const str = timerListToString(list);
  set('timer_config', str);
  // Keep the Clay config page in sync (clay-settings holds last-saved values that
  // generateUrl bakes into the page); otherwise a later Save would drop this timer.
  let cs: any = {};
  try { cs = JSON.parse(get('clay-settings') || '{}') || {}; } catch (e) { cs = {}; }
  cs.TimerList = list;
  set('clay-settings', JSON.stringify(cs));
  return str;
}
```

- [ ] **Step 4: Run the test to verify it passes**

Run: `npm test`
Expected: PASS — all `add_timer` tests green (and the existing `config_sync` / `timer_config` tests still pass).

- [ ] **Step 5: Commit**

```bash
git add src/ts/add_timer.ts tests/add_timer.test.js
git commit -m "Phone: appendCustomTimer helper (timer_config + clay-settings)"
```

---

### Task 6: Wire `AddTimer` into the phone `appmessage` listener

**Files:**
- Modify: `src/ts/index.ts:14-20` (the `appmessage` listener)

- [ ] **Step 1: Update the listener**

In `src/ts/index.ts`, add the import near the top with the other imports:

```ts
import { appendCustomTimer } from './add_timer';
```

Replace the existing `Pebble.addEventListener('appmessage', ...)` block with:

```ts
// Inbound from the watch: either a custom timer to save (AddTimer), or the launch
// handshake (any other message) where we resend the last-saved config.
Pebble.addEventListener('appmessage', (e: any) => {
  const p = e && e.payload;
  if (p && typeof p.AddTimer === 'number') {
    appendCustomTimer(
      (k) => window.localStorage.getItem(k),
      (k, v) => window.localStorage.setItem(k, v),
      p.AddTimer);
    console.log('AddTimer saved: ' + p.AddTimer + 's');
    return;   // no echo — the watch already holds the running timer locally as custom
  }
  const dict = resendDict((k) => window.localStorage.getItem(k));
  if (!dict) { return; }
  Pebble.sendAppMessage(dict, () => { console.log('config resent'); },
    () => { console.log('config resend failed'); });
});
```

- [ ] **Step 2: Typecheck + tests**

Run: `npm run typecheck && npm test`
Expected: PASS (no type errors; tests unchanged from Task 5 still green).

- [ ] **Step 3: Commit**

```bash
git add src/ts/index.ts
git commit -m "Phone: handle AddTimer inbound (save custom timer)"
```

---

### Task 7: List long-SELECT opens the detail window in any state

**Files:**
- Modify: `src/c/main.c` (`window_load` callbacks ~line 588; add `ml_select_long` near `ml_select`)

- [ ] **Step 1: Add the long-click handler**

In `src/c/main.c`, immediately after `ml_select` (it ends near line 540), add:

```c
// Long SELECT opens the detail window for ANY timer (short SELECT still starts an
// idle timer directly). This is how an idle/done timer reaches the +/- adjust and
// the "Save as new & start" action.
static void ml_select_long(MenuLayer *ml, MenuIndex *ci, void *ctx) {
  if (s_count == 0) { return; }
  open_detail_window(s_order[ci->row]);
}
```

- [ ] **Step 2: Register it on the list MenuLayer**

In `window_load`, add the `.select_long_click` field to the `ml_*` callbacks struct:

```c
  menu_layer_set_callbacks(s_menu, NULL, (MenuLayerCallbacks){
    .get_num_rows = ml_num_rows,
    .get_cell_height = ml_cell_height,
    .draw_row = ml_draw_row,
    .select_click = ml_select,
    .select_long_click = ml_select_long,
  });
```

- [ ] **Step 3: Build**

Run: `pebble build`
Expected: build succeeds.

- [ ] **Step 4: Commit**

```bash
git add src/c/main.c
git commit -m "List: long SELECT opens detail window in any state"
```

---

### Task 8: Detail window rows per state + idle/done +/- adjust

**Files:**
- Modify: `src/c/main.c` (`dl_num_rows`, `dl_row_label`, `dl_draw_row`, `dl_select`, `detail_addable`)

- [ ] **Step 1: Replace the row-count + label helpers**

In `src/c/main.c`, replace `detail_addable`, `dl_num_rows`, and `dl_row_label` with:

```c
// Startable = the detail window's timer can be started fresh (idle or done); these
// states offer "Save as new & start" instead of "Stop".
static bool detail_startable(void) {
  if (s_detail_idx < 0 || s_detail_idx >= s_count) { return false; }
  TimerState st = s_timers[s_detail_idx].state;
  return st == TS_IDLE || st == TS_DONE;
}

static uint16_t dl_num_rows(MenuLayer *ml, uint16_t section, void *ctx) {
  return 4;   // row0 primary, row1 secondary, +1 min, -1 min
}
static int16_t dl_cell_height(MenuLayer *ml, MenuIndex *ci, void *ctx) { return 28; }
static int16_t dl_header_height(MenuLayer *ml, uint16_t section, void *ctx) { return 26; }

static const char *dl_row_label(int row, TimerState st) {
  switch (row) {
    case 0: return (st == TS_RUNNING) ? "Pause" : "Start";   // paused/idle/done -> Start(=resume/start)
    case 1: return (st == TS_IDLE || st == TS_DONE) ? "Save as new & start" : "Stop";
    case 2: return "+1 min";
    case 3: return "-1 min";
    default: return "";
  }
}
```

- [ ] **Step 2: Update `dl_draw_row` to pass the state**

Replace the body of `dl_draw_row` with:

```c
static void dl_draw_row(GContext *gctx, const Layer *cell, MenuIndex *ci, void *ctx) {
  TimerState st = (s_detail_idx >= 0 && s_detail_idx < s_count)
                  ? s_timers[s_detail_idx].state : TS_IDLE;
  GRect b = layer_get_bounds(cell);
  graphics_draw_text(gctx, dl_row_label(ci->row, st),
    fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
    GRect(6, 1, b.size.w - 12, b.size.h - 1),
    GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
}
```

- [ ] **Step 3: Update `dl_select` for the new rows + idle/done adjust**

Replace the body of `dl_select` with (the `Save as new & start` branch calls `save_as_new_and_start`, added in Task 9 — add a forward declaration now so this compiles):

```c
static void save_as_new_and_start(int32_t secs);  // defined in Task 9

static void dl_select(MenuLayer *ml, MenuIndex *ci, void *ctx) {
  int idx = s_detail_idx;
  if (idx < 0 || idx >= s_count) { return; }
  Timer *t = &s_timers[idx];
  if (ci->row == 0) {                 // Pause / Start(resume/start) -> keep window open
    if (t->state == TS_RUNNING) { tc_pause(t, now_s()); }
    else { tc_start(t, now_s()); }
    persist_all(); rearm_wakeup(); ensure_ticking();
    reload_ui(); menu_layer_reload_data(s_detail_menu);
  } else if (ci->row == 1 && detail_startable()) {   // Save as new & start
    save_as_new_and_start(tc_remaining_now(t, now_s()) >= 1
                          ? tc_remaining_now(t, now_s()) : t->duration);
  } else if (ci->row == 1) {          // Stop (reset) -- running/paused
    tc_reset(t, now_s());
    persist_all(); rearm_wakeup(); reload_ui(); select_timer_row(idx);
    if (s_auto_return) { window_stack_pop_all(true); }
    else { window_stack_remove(s_detail_window, true); }
  } else {                            // row 2/3: +1 / -1 min
    int32_t secs = (ci->row == 2) ? 60 : -60;
    if (t->state == TS_RUNNING || t->state == TS_PAUSED) {
      tc_add(t, secs, now_s());
    } else {                          // idle/done: adjust `remaining` (60s floor), keep template
      int32_t r = t->remaining + secs;
      if (r < 60) { r = 60; }
      t->remaining = r;
      t->last_used = now_s();
    }
    persist_all(); rearm_wakeup(); ensure_ticking();
    reload_ui(); menu_layer_reload_data(s_detail_menu);
  }
}
```

- [ ] **Step 4: Build**

Run: `pebble build`
Expected: build succeeds (the forward-declared `save_as_new_and_start` resolves once Task 9 defines it; if building this task alone, add a temporary empty stub `static void save_as_new_and_start(int32_t secs) {}` and remove it in Task 9).

- [ ] **Step 5: Commit**

```bash
git add src/c/main.c
git commit -m "Detail: state-aware rows + Save-as-new row + idle/done +/- adjust"
```

---

### Task 9: Implement `save_as_new_and_start` (create + AddTimer + start tail)

**Files:**
- Modify: `src/c/main.c` (define `save_as_new_and_start`; add an `AddTimer` outbox sender)

- [ ] **Step 1: Add the outbox sender**

In `src/c/main.c`, near `request_config` (around line 575), add:

```c
// Tell the phone to save a new unnamed timer of `secs` seconds (appended to its
// TimerConfig + Clay store). The watch keeps the running timer locally (flagged
// custom) so it survives even if this send fails / the phone is offline.
static void send_add_timer(int32_t secs) {
  DictionaryIterator *out;
  if (app_message_outbox_begin(&out) == APP_MSG_OK) {
    dict_write_uint32(out, MESSAGE_KEY_AddTimer, (uint32_t)secs);
    app_message_outbox_send();
  }
}
```

- [ ] **Step 2: Define `save_as_new_and_start`**

In `src/c/main.c`, KEEP the forward declaration `static void save_as_new_and_start(int32_t secs);` (added before `dl_select` in Task 8 — `dl_select` calls it). Add the DEFINITION lower in the file, after both `show_start_confirmation` (it calls it) and `send_add_timer` (Step 1 above). If Task 8 used a temporary empty stub instead of a forward declaration, delete that stub now:

```c
// Create a NEW unnamed timer of `secs`, started now, appended at the end of the
// list (so a later config reconcile aligns the phone's appended entry to this
// running row by position). Persist, send AddTimer, then apply the normal start
// tail (confirmation + auto-return).
static void save_as_new_and_start(int32_t secs) {
  if (s_count >= MAX_TIMERS) {
    // List full: nothing to create. (Keep it simple — no new row.)
    return;
  }
  if (secs < 1) { secs = 1; }
  int idx = s_count;
  Timer *t = &s_timers[idx];
  memset(t, 0, sizeof(*t));
  t->name[0] = 0;
  t->duration = secs;
  t->remaining = secs;
  t->state = TS_IDLE;
  t->custom = true;
  tc_start(t, now_s());            // -> RUNNING, end_time = now + secs
  s_count++;
  persist_all(); rearm_wakeup(); ensure_ticking();
  send_add_timer(secs);
  reload_ui();
  select_timer_row(idx);
  if (s_auto_return) { show_start_confirmation(idx); }   // flash -> watchface
  else { window_stack_remove(s_detail_window, true); }   // back to the list
}
```

- [ ] **Step 3: Build**

Run: `pebble clean && pebble build`
Expected: build succeeds; no `MESSAGE_KEY_AddTimer undeclared` (regenerated in Task 4).

- [ ] **Step 4: Commit**

```bash
git add src/c/main.c
git commit -m "Detail: Save as new & start -> create custom timer + AddTimer"
```

---

### Task 10: Verify on the emulator + bump the gitlink

**Files:**
- Verification only, then `src/c` build artifacts ignored; superrepo gitlink bump.

- [ ] **Step 1: Boot the emulator (user's board is Pebble Time 2 -> emery)**

Run: `scripts/pebble-emu-boot.sh emery`
Expected: boots within 1-2 attempts.

- [ ] **Step 2: Install with a seeded timer to exercise the flow**

Run:
```bash
PEBBLE_QEMU_PATH=~/.local/bin/qemu-pebble-headless pebble build
PEBBLE_QEMU_PATH=~/.local/bin/qemu-pebble-headless pebble install --emulator emery
```
Expected: `App install succeeded`. (The list shows the `SCREENSHOT_FIXTURES` seed timers Egg/Tea/Laundry when no phone config is present.)

- [ ] **Step 3: Screenshot the idle detail window**

Long-SELECT cannot be driven headlessly via the screenshot tool, so verify the detail rows by temporarily forcing the detail window open on an idle timer at init (or trust the build + on-watch check in Step 5). If forcing: in `init()` after `window_stack_push(s_window, true)` temporarily add `open_detail_window(1);`, build/install/screenshot, then REVERT.

Run: `PEBBLE_QEMU_PATH=~/.local/bin/qemu-pebble-headless pebble screenshot --no-open /tmp/detail.png`
Expected: rows read `Start` / `Save as new & start` / `+1 min` / `-1 min`. Surface the screenshot to the user.

- [ ] **Step 4: Run the full test suites once more**

Run: `gcc -I src/c tests/test_timer_calc.c src/c/timer_calc.c -o /tmp/tct && /tmp/tct && npm test`
Expected: both green.

- [ ] **Step 5: Real-watch check via CloudPebble (manual, with the user)**

Run: `pebble install --cloudpebble build/*.pbw` and `pebble logs --cloudpebble` (separate session).
Confirm: long-SELECT idle timer → adjust → "Save as new & start" appends an unnamed running row; `AddTimer saved` appears in phone logs; the timer survives an app close/reopen; it shows on the Clay config page.

- [ ] **Step 6: Commit the gitlink bump in the superrepo**

```bash
cd /home/dev/pebble-timetracking
git add PebbleCountdownTimer
git commit -m "Bump PebbleCountdownTimer: quick custom timer"
```

---

## Notes for the implementer

- **Detached HEAD:** inside the submodule run `git checkout master` before working so commits land on the branch (see CLAUDE.md submodule section).
- **`pebble clean` is required** after the `messageKeys` edit (Task 4) or the C side fails with `MESSAGE_KEY_AddTimer undeclared` (cached `message_keys.json`).
- **`src/pkjs/*.js` is generated** from `src/ts/*.ts` by `tsc`; never edit the JS. `npm test`'s `pretest` runs `tsc`, and tests `require('../src/pkjs/...')` — the generated path.
- **`clay-settings` key:** the helper assumes Clay persists last-saved values under `localStorage['clay-settings']` as `{ messageKey: value }`. If the real key/shape differs on this watch, only the config-page mirroring is affected (the `timer_config` append — what the watch reads on launch — is independent and correct). Verify by logging `window.localStorage.getItem('clay-settings')` after a Save during Step 5.
```
