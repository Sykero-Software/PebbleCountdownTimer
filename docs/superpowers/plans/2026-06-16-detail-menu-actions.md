# Detail-menu actions Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make Stop the default detail-window action, stop saving duplicate timers on an unchanged "Start & Save", add a Delete action with phone sync, and make every app exit return to the watchface.

**Architecture:** The fixed 4-row detail menu becomes a per-state action list built by a new pure helper in `timer_calc.c` (unit-testable without the SDK). `main.c` drives `dl_num_rows`/`dl_draw_row`/`dl_select` off that list. Delete adds a new `DeleteTimer` AppMessage key and a PKJS module that mirrors `add_timer.ts`. A `close_to_watchface()` helper centralises `exit_reason_set` before `window_stack_pop_all`.

**Tech Stack:** Pebble SDK (C, watchapp), TypeScript→`src/pkjs` (PKJS), `node:test` for PKJS, plain `gcc`-compiled assertions for the C core.

**Spec:** `docs/superpowers/specs/2026-06-16-detail-menu-actions-design.md`

---

### Task 1: Detail-action list helpers in `timer_calc`

**Files:**
- Modify: `src/c/timer_calc.h`
- Modify: `src/c/timer_calc.c`
- Test: `tests/test_timer_calc.c`

- [ ] **Step 1: Add the failing tests**

Insert before the final `printf("All timer_calc tests passed\n");` in `tests/test_timer_calc.c`:

```c
  // --- tc_detail_changed: only idle/done with a tuned remaining count as "changed" ---
  Timer dc; memset(&dc, 0, sizeof(dc)); dc.duration = 300;
  dc.state = TS_IDLE; dc.remaining = 300; assert(tc_detail_changed(&dc) == false);
  dc.remaining = 360; assert(tc_detail_changed(&dc) == true);   // +1 min
  dc.state = TS_DONE; dc.remaining = 0;   assert(tc_detail_changed(&dc) == false); // done untouched -> falls back to duration
  dc.remaining = 60;  assert(tc_detail_changed(&dc) == true);
  dc.state = TS_RUNNING; dc.remaining = 999; assert(tc_detail_changed(&dc) == false); // running never "changed"
  dc.state = TS_PAUSED;  assert(tc_detail_changed(&dc) == false);

  // --- tc_detail_actions: ordered list per state (Stop first for running/paused, Delete last) ---
  DetailAction acts[7]; int an;
  an = tc_detail_actions(TS_RUNNING, false, acts);
  assert(an == 5 && acts[0] == DACT_STOP && acts[1] == DACT_PAUSE &&
         acts[2] == DACT_PLUS && acts[3] == DACT_MINUS && acts[4] == DACT_DELETE);
  an = tc_detail_actions(TS_PAUSED, false, acts);
  assert(an == 5 && acts[0] == DACT_STOP && acts[1] == DACT_START &&
         acts[2] == DACT_PLUS && acts[3] == DACT_MINUS && acts[4] == DACT_DELETE);
  an = tc_detail_actions(TS_IDLE, false, acts);   // unchanged -> no Save row
  assert(an == 4 && acts[0] == DACT_START && acts[1] == DACT_PLUS &&
         acts[2] == DACT_MINUS && acts[3] == DACT_DELETE);
  an = tc_detail_actions(TS_IDLE, true, acts);    // changed -> Save row at index 1
  assert(an == 5 && acts[0] == DACT_START && acts[1] == DACT_SAVE_START &&
         acts[2] == DACT_PLUS && acts[3] == DACT_MINUS && acts[4] == DACT_DELETE);
  an = tc_detail_actions(TS_DONE, true, acts);
  assert(an == 5 && acts[0] == DACT_START && acts[1] == DACT_SAVE_START);
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cd PebbleCountdownTimer && gcc -I src/c tests/test_timer_calc.c src/c/timer_calc.c -o /tmp/t && /tmp/t`
Expected: FAIL to compile — `unknown type name 'DetailAction'` / `DACT_STOP undeclared` / `tc_detail_actions` implicit declaration.

- [ ] **Step 3: Declare the enum and helpers in `timer_calc.h`**

Add after the `SortMode` enum (around line 14) in `src/c/timer_calc.h`:

```c
// Detail-window actions, in no particular order; tc_detail_actions() returns them
// in display order per timer state.
typedef enum {
  DACT_STOP,        // running/paused -> reset to IDLE
  DACT_PAUSE,       // running -> pause
  DACT_START,       // idle/done/paused -> start or resume in place
  DACT_SAVE_START,  // idle/done with a tuned time -> create a new custom timer + start
  DACT_PLUS,        // +1 min
  DACT_MINUS,       // -1 min
  DACT_DELETE,      // delete this timer (after a confirm screen)
} DetailAction;
```

Add at the end of `src/c/timer_calc.h` (before the final blank line):

```c
// True when an idle/done timer's start time was tuned away from its template
// duration (i.e. the user pressed +/- before starting), so "Start & Save" is
// meaningful. Always false for RUNNING/PAUSED.
bool tc_detail_changed(const Timer *t);

// Fill `out` (capacity >= 7) with the ordered detail-window actions for a timer in
// state `st` whose duration was/was not `changed`. Returns the number written.
int tc_detail_actions(TimerState st, bool changed, DetailAction *out);
```

- [ ] **Step 4: Implement the helpers in `timer_calc.c`**

Add at the end of `src/c/timer_calc.c`:

```c
bool tc_detail_changed(const Timer *t) {
  if (t->state != TS_IDLE && t->state != TS_DONE) { return false; }
  int32_t intended = (t->remaining >= 1) ? t->remaining : t->duration;
  return intended != t->duration;
}

int tc_detail_actions(TimerState st, bool changed, DetailAction *out) {
  int n = 0;
  if (st == TS_RUNNING) {
    out[n++] = DACT_STOP;
    out[n++] = DACT_PAUSE;
  } else if (st == TS_PAUSED) {
    out[n++] = DACT_STOP;
    out[n++] = DACT_START;   // resume
  } else {                   // TS_IDLE / TS_DONE
    out[n++] = DACT_START;
    if (changed) { out[n++] = DACT_SAVE_START; }
  }
  out[n++] = DACT_PLUS;
  out[n++] = DACT_MINUS;
  out[n++] = DACT_DELETE;
  return n;
}
```

- [ ] **Step 5: Run the test to verify it passes**

Run: `cd PebbleCountdownTimer && gcc -I src/c tests/test_timer_calc.c src/c/timer_calc.c -o /tmp/t && /tmp/t`
Expected: PASS — prints `All timer_calc tests passed`.

- [ ] **Step 6: Commit**

```bash
cd PebbleCountdownTimer
git add src/c/timer_calc.h src/c/timer_calc.c tests/test_timer_calc.c
git commit -m "feat(timer): detail-action list helpers (tc_detail_actions/tc_detail_changed)"
```

---

### Task 2: Exit to the watchface (`close_to_watchface`)

**Files:**
- Modify: `src/c/main.c`

No unit test (SDK-only `exit_reason_set`); verified by build + manual.

- [ ] **Step 1: Add the helper**

In `src/c/main.c`, add immediately after `static int64_t now_s(...)` (around line 50):

```c
// Close the app to the WATCHFACE (not the launcher). exit_reason_set tells
// PebbleOS this was a completed action, so it returns to the watchface; without
// it window_stack_pop_all lands back wherever the app was launched from.
static void close_to_watchface(void) {
  exit_reason_set(APP_EXIT_ACTION_PERFORMED_SUCCESSFULLY);
  window_stack_pop_all(true);
}
```

- [ ] **Step 2: Route `alarm_stop` through it**

In `alarm_stop` (around line 134), replace:

```c
  window_stack_pop_all(true);
```
with:
```c
  close_to_watchface();
```

- [ ] **Step 3: Route the detail-window Stop auto-return through it**

In `dl_select` (around line 348), replace:

```c
    if (s_auto_return) { window_stack_pop_all(true); }
```
with:
```c
    if (s_auto_return) { close_to_watchface(); }
```

- [ ] **Step 4: Route the "Started" confirmation auto-close through it**

In `confirm_timer_cb` (around line 462), replace:

```c
  window_stack_pop_all(true);   // close the app -> watchface
```
with:
```c
  close_to_watchface();   // -> watchface (with exit reason)
```

- [ ] **Step 5: Build to verify it compiles**

Run: `cd PebbleCountdownTimer && PEBBLE_QEMU_PATH=~/.local/bin/qemu-pebble-headless pebble build 2>&1 | tail -5`
Expected: `'build' finished successfully` (no `exit_reason_set` undeclared error).

- [ ] **Step 6: Commit**

```bash
cd PebbleCountdownTimer
git add src/c/main.c
git commit -m "fix(exit): set APP_EXIT_ACTION_PERFORMED_SUCCESSFULLY on all app-closing exits"
```

---

### Task 3: Detail menu driven by the action list (Stop default + no-dup save)

**Files:**
- Modify: `src/c/main.c`

This replaces the hardcoded 4-row detail menu (`dl_num_rows`, `dl_row_label`,
`dl_draw_row`, `dl_select`, `detail_startable`) with the action-list model.
Verified by build + manual (the row logic itself is covered by Task 1's tests).

- [ ] **Step 1: Add the action-list cache + helpers**

In `src/c/main.c`, just below the detail-window state declarations (after
`static int s_detail_idx = -1;`, around line 39), add:

```c
static DetailAction s_detail_acts[7];   // rebuilt per reload by dl_rebuild_actions
static int s_detail_act_count = 0;
```

Replace the whole `detail_startable` function (around lines 278-284) with these
helpers (it is no longer used anywhere else):

```c
static void dl_rebuild_actions(void) {
  if (s_detail_idx < 0 || s_detail_idx >= s_count) { s_detail_act_count = 0; return; }
  Timer *t = &s_timers[s_detail_idx];
  s_detail_act_count = tc_detail_actions(t->state, tc_detail_changed(t), s_detail_acts);
}

// Move the detail cursor onto the row that now carries action `a` (the list can
// grow when +/- introduces the Save row, so a repeated +/- press must follow its
// button instead of landing on whatever shifted under the cursor).
static void dl_select_action(DetailAction a) {
  if (!s_detail_menu) { return; }
  for (int r = 0; r < s_detail_act_count; r++) {
    if (s_detail_acts[r] == a) {
      menu_layer_set_selected_index(s_detail_menu,
        (MenuIndex){ .section = 0, .row = (uint16_t)r }, MenuRowAlignNone, false);
      return;
    }
  }
}

static const char *dl_action_label(DetailAction a) {
  switch (a) {
    case DACT_STOP:       return "Stop";
    case DACT_PAUSE:      return "Pause";
    case DACT_START:      return "Start";
    case DACT_SAVE_START: return "Start & Save";
    case DACT_PLUS:       return "+1 min";
    case DACT_MINUS:      return "-1 min";
    case DACT_DELETE:     return "Delete";
  }
  return "";
}
```

- [ ] **Step 2: Rewrite `dl_num_rows` and `dl_draw_row`**

Replace `dl_num_rows` (around line 286) with:

```c
static uint16_t dl_num_rows(MenuLayer *ml, uint16_t section, void *ctx) {
  dl_rebuild_actions();
  return (uint16_t)s_detail_act_count;
}
```

Delete the `dl_row_label` function entirely (around lines 311-319).

Replace `dl_draw_row` (around lines 321-329) with:

```c
static void dl_draw_row(GContext *gctx, const Layer *cell, MenuIndex *ci, void *ctx) {
  if (ci->row >= s_detail_act_count) { return; }
  GRect b = layer_get_bounds(cell);
  graphics_draw_text(gctx, dl_action_label(s_detail_acts[ci->row]),
    fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
    GRect(6, 1, b.size.w - 12, b.size.h - 1),
    GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
}
```

- [ ] **Step 3: Add forward declarations for the delete path**

Just above `dl_select` (around line 333, next to the existing
`static void save_as_new_and_start(int32_t secs);`), add:

```c
static void open_delete_confirm(void);   // defined in Task 5
```

- [ ] **Step 4: Rewrite `dl_select` to dispatch on the action**

Replace the whole `dl_select` function (around lines 333-363) with:

```c
static void dl_select(MenuLayer *ml, MenuIndex *ci, void *ctx) {
  int idx = s_detail_idx;
  if (idx < 0 || idx >= s_count) { return; }
  dl_rebuild_actions();
  if (ci->row >= s_detail_act_count) { return; }
  DetailAction a = s_detail_acts[ci->row];
  Timer *t = &s_timers[idx];
  switch (a) {
    case DACT_PAUSE:
      tc_pause(t, now_s());
      persist_all(); rearm_wakeup(); ensure_ticking();
      reload_ui(); menu_layer_reload_data(s_detail_menu);
      break;
    case DACT_START:                    // start (idle/done) or resume (paused), in place
      tc_start(t, now_s());
      persist_all(); rearm_wakeup(); ensure_ticking();
      reload_ui(); menu_layer_reload_data(s_detail_menu);
      break;
    case DACT_SAVE_START: {             // only present when the time was tuned -> never a dup
      int32_t rem = tc_remaining_now(t, now_s());
      save_as_new_and_start(rem >= 1 ? rem : t->duration);
      break;
    }
    case DACT_STOP:                     // reset to idle
      tc_reset(t, now_s());
      persist_all(); rearm_wakeup(); reload_ui(); select_timer_row(idx);
      if (s_auto_return) { close_to_watchface(); }
      else { window_stack_remove(s_detail_window, true); }
      break;
    case DACT_PLUS:
    case DACT_MINUS: {
      int32_t secs = (a == DACT_PLUS) ? 60 : -60;
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
      dl_select_action(a);              // keep the cursor on the +/- button as the list grows
      break;
    }
    case DACT_DELETE:
      open_delete_confirm();
      break;
  }
}
```

- [ ] **Step 5: Build to verify it compiles**

Run: `cd PebbleCountdownTimer && PEBBLE_QEMU_PATH=~/.local/bin/qemu-pebble-headless pebble build 2>&1 | tail -5`
Expected: `'build' finished successfully`. (`open_delete_confirm` is forward-declared; it is defined in Task 5. If linking fails here because the build is a full link, do Task 5 before building — see note.) Since `pebble build` compiles and links the app, **this build will FAIL to link with `undefined reference to open_delete_confirm`** — that is expected; the compile of `main.c` itself must produce only the link error, no other errors. Confirm the only error is the missing `open_delete_confirm`, then proceed; the successful build happens at the end of Task 5.

- [ ] **Step 6: Commit**

```bash
cd PebbleCountdownTimer
git add src/c/main.c
git commit -m "feat(detail): action-list menu — Stop default, Save row only when tuned"
```

---

### Task 4: `DeleteTimer` message key + PKJS delete module

**Files:**
- Modify: `package.json`
- Create: `src/ts/delete_timer.ts`
- Modify: `src/ts/index.ts`
- Test: `tests/delete_timer.test.js`

- [ ] **Step 1: Write the failing test**

Create `tests/delete_timer.test.js`:

```js
// SPDX-License-Identifier: GPL-3.0-only
const test = require('node:test');
const assert = require('node:assert');
const { deleteTimer } = require('../src/pkjs/delete_timer');

function fakeStore(init) {
  const m = new Map(Object.entries(init || {}));
  return {
    get: (k) => (m.has(k) ? m.get(k) : null),
    set: (k, v) => m.set(k, v),
    dump: () => m,
  };
}
const RS = '\x1e', US = '\x1f';

test('removes the timer at the given index from timer_config', () => {
  const s = fakeStore({ timer_config: 'Egg' + US + '300' + RS + 'Tea' + US + '120' + RS + US + '90' });
  const str = deleteTimer(s.get, s.set, 1);   // drop "Tea"
  assert.strictEqual(str, 'Egg' + US + '300' + RS + US + '90');
  assert.strictEqual(s.get('timer_config'), str);
});

test('mirrors the deletion into the clay-settings TimerList', () => {
  const s = fakeStore({
    timer_config: 'Egg' + US + '300' + RS + 'Tea' + US + '120',
    'clay-settings': JSON.stringify({ SortOrder: '0' }),
  });
  deleteTimer(s.get, s.set, 0);   // drop "Egg"
  const cs = JSON.parse(s.get('clay-settings'));
  assert.deepStrictEqual(cs.TimerList, [{ name: 'Tea', seconds: 120 }]);
  assert.strictEqual(cs.SortOrder, '0'); // other keys preserved
});

test('out-of-range index returns null and leaves storage untouched', () => {
  const s = fakeStore({ timer_config: 'Egg' + US + '300' });
  assert.strictEqual(deleteTimer(s.get, s.set, 5), null);
  assert.strictEqual(deleteTimer(s.get, s.set, -1), null);
  assert.strictEqual(s.get('timer_config'), 'Egg' + US + '300');
});
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cd PebbleCountdownTimer && npm test 2>&1 | tail -15`
Expected: FAIL — `Cannot find module '../src/pkjs/delete_timer'` (the `pretest` `tsc` has nothing to compile yet).

- [ ] **Step 3: Implement `src/ts/delete_timer.ts`**

```ts
// SPDX-License-Identifier: GPL-3.0-only

// Remove a watch-selected timer (by list index) from the phone's persisted config
// and the Clay config-page store, mirroring add_timer.ts. `get`/`set` read/write
// localStorage by key. Returns the new TimerConfig string, or null when `index`
// is out of range (storage left untouched in that case).
import { stringToTimerList, timerListToString } from './timer_config';

export function deleteTimer(
  get: (k: string) => string | null,
  set: (k: string, v: string) => void,
  index: any
): string | null {
  const i = Math.floor(Number(index));
  const list = stringToTimerList(get('timer_config') || '');
  if (!(i >= 0 && i < list.length)) { return null; }
  list.splice(i, 1);
  const str = timerListToString(list);
  set('timer_config', str);
  let cs: any = {};
  try { cs = JSON.parse(get('clay-settings') || '{}') || {}; } catch (e) { cs = {}; }
  cs.TimerList = list;
  set('clay-settings', JSON.stringify(cs));
  return str;
}
```

- [ ] **Step 4: Run the test to verify it passes**

Run: `cd PebbleCountdownTimer && npm test 2>&1 | tail -15`
Expected: PASS — the three `delete_timer` tests plus the existing suites pass.

- [ ] **Step 5: Handle `DeleteTimer` in `index.ts`**

In `src/ts/index.ts`, add the import next to the others (after line 9):

```ts
import { deleteTimer } from './delete_timer';
```

Inside the `Pebble.addEventListener('appmessage', ...)` handler, add this branch
immediately after the existing `AddTimer` block's `return;` (after line 25),
before the `resendDict` fallback:

```ts
  if (p && typeof p.DeleteTimer === 'number') {
    const left = deleteTimer(
      (k) => window.localStorage.getItem(k),
      (k, v) => window.localStorage.setItem(k, v),
      p.DeleteTimer);
    console.log(left === null ? 'DeleteTimer rejected (out of range): ' + p.DeleteTimer
      : 'DeleteTimer applied at index ' + p.DeleteTimer);
    return;   // no echo — the watch already removed it locally
  }
```

- [ ] **Step 6: Append the `DeleteTimer` message key**

In `package.json`, change the `messageKeys` line (line 18) to append `DeleteTimer`
**last** (append-only — never reorder existing keys):

```json
    "messageKeys": ["TimerConfig", "SortOrder", "Request", "AutoReturn", "RunningFirst", "AddTimer", "DeleteTimer"],
```

- [ ] **Step 7: Run the test again (tsc still compiles `index.ts` cleanly)**

Run: `cd PebbleCountdownTimer && npm test 2>&1 | tail -15`
Expected: PASS (the `pretest` `tsc` compiles `index.ts` + `delete_timer.ts` with no type errors).

- [ ] **Step 8: Commit**

```bash
cd PebbleCountdownTimer
git add package.json src/ts/delete_timer.ts src/ts/index.ts src/pkjs/delete_timer.js src/pkjs/index.js tests/delete_timer.test.js
git commit -m "feat(delete): DeleteTimer message key + PKJS deleteTimer sync"
```

---

### Task 5: Delete on the watch (confirm window + local removal + send)

**Files:**
- Modify: `src/c/main.c`

Completes the `open_delete_confirm` forward-declared in Task 3. Verified by the
final clean build + manual emulator check (Task 6).

- [ ] **Step 1: Add the local-removal helper + the AddTimer-style sender**

In `src/c/main.c`, add `remove_timer_at` just above `save_as_new_and_start`
(around line 615):

```c
// Remove the timer at `idx`, shifting the tail down. Caller persists + re-sorts.
static void remove_timer_at(int idx) {
  if (idx < 0 || idx >= s_count) { return; }
  for (int i = idx; i < s_count - 1; i++) { s_timers[i] = s_timers[i + 1]; }
  s_count--;
}
```

Add `send_delete_timer` immediately after `send_add_timer` (around line 610):

```c
// Tell the phone to drop the timer at list index `idx` from its TimerConfig +
// Clay store. Best-effort, like send_add_timer: if it fails (phone offline) the
// watch still removes it locally, but a later config reconcile will re-add it.
static void send_delete_timer(int32_t idx) {
  DictionaryIterator *out;
  if (app_message_outbox_begin(&out) == APP_MSG_OK) {
    dict_write_int32(out, MESSAGE_KEY_DeleteTimer, idx);
    app_message_outbox_send();
  }
}
```

- [ ] **Step 2: Add the delete-confirm window**

In `src/c/main.c`, add the confirm-window state next to the other window globals
(after the `s_confirm_*` block, around line 48):

```c
// ---- delete-confirm window: "Delete? <name>" + Select=delete / Back=cancel ----
static Window  *s_del_window;
static Layer   *s_del_layer;
static char     s_del_name[NAME_LEN + 1];
```

Add the window implementation just above `open_detail_window` (around line 386):

```c
static void del_update_proc(Layer *layer, GContext *gctx) {
  GRect b = layer_get_bounds(layer);
  int cy = b.size.h / 2;
  graphics_context_set_text_color(gctx, GColorBlack);
  graphics_draw_text(gctx, "Delete?", fonts_get_system_font(FONT_KEY_GOTHIC_28_BOLD),
    GRect(4, cy - 52, b.size.w - 8, 32), GTextOverflowModeFill, GTextAlignmentCenter, NULL);
  graphics_draw_text(gctx, s_del_name, fonts_get_system_font(FONT_KEY_GOTHIC_24),
    GRect(4, cy - 16, b.size.w - 8, 30), GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
  graphics_draw_text(gctx, "SELECT delete\nBACK cancel", fonts_get_system_font(FONT_KEY_GOTHIC_18),
    GRect(4, cy + 22, b.size.w - 8, 44), GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);
}

static void del_window_load(Window *w) {
  Layer *root = window_get_root_layer(w);
  s_del_layer = layer_create(layer_get_bounds(root));
  layer_set_update_proc(s_del_layer, del_update_proc);
  layer_add_child(root, s_del_layer);
}

static void del_window_unload(Window *w) {
  if (s_del_layer) { layer_destroy(s_del_layer); s_del_layer = NULL; }
}

// Select confirms: tell the phone, remove locally, then drop both the confirm and
// the detail window so we land back on the LIST (delete is management, not an exit).
static void del_confirm_select(ClickRecognizerRef rec, void *ctx) {
  int idx = s_detail_idx;
  if (idx >= 0 && idx < s_count) {
    send_delete_timer(idx);
    remove_timer_at(idx);
    persist_all(); rearm_wakeup(); reload_ui();
  }
  s_detail_idx = -1;
  window_stack_remove(s_del_window, false);
  window_stack_remove(s_detail_window, true);
}

// Back is the implicit pop (cancel); only Select needs a handler.
static void del_click_config(void *ctx) {
  window_single_click_subscribe(BUTTON_ID_SELECT, del_confirm_select);
}

static void open_delete_confirm(void) {
  if (s_detail_idx < 0 || s_detail_idx >= s_count) { return; }
  Timer *t = &s_timers[s_detail_idx];
  if (t->name[0]) {
    snprintf(s_del_name, sizeof(s_del_name), "%s", t->name);
  } else {
    tc_format_remaining(s_del_name, sizeof(s_del_name), tc_remaining_now(t, now_s()));
  }
  if (!s_del_window) {
    s_del_window = window_create();
    window_set_window_handlers(s_del_window, (WindowHandlers){
      .load = del_window_load, .unload = del_window_unload });
    window_set_click_config_provider(s_del_window, del_click_config);
  }
  window_stack_push(s_del_window, true);
}
```

Note: `send_delete_timer` and `remove_timer_at` are defined later in the file than
`open_delete_confirm`. Add forward declarations next to the existing
`static void save_as_new_and_start(int32_t secs);` (around line 331):

```c
static void send_delete_timer(int32_t idx);
static void remove_timer_at(int idx);
```

- [ ] **Step 3: Destroy the confirm window in `deinit`**

In `deinit` (around line 694), add next to the `s_confirm_window` cleanup:

```c
  if (s_del_window) { window_destroy(s_del_window); }
```

- [ ] **Step 4: Regenerate message-key macros + build**

The new `MESSAGE_KEY_DeleteTimer` C macro is generated from `package.json`; a plain
`pebble build` reuses the cached header, so `clean` first.

Run: `cd PebbleCountdownTimer && PEBBLE_QEMU_PATH=~/.local/bin/qemu-pebble-headless pebble clean && PEBBLE_QEMU_PATH=~/.local/bin/qemu-pebble-headless pebble build 2>&1 | tail -8`
Expected: `'build' finished successfully`, no `'MESSAGE_KEY_DeleteTimer' undeclared` and no `undefined reference to open_delete_confirm`.

- [ ] **Step 5: Commit**

```bash
cd PebbleCountdownTimer
git add src/c/main.c
git commit -m "feat(delete): on-watch delete with confirm screen + phone DeleteTimer send"
```

---

### Task 6: Full verification + gitlink bump

**Files:**
- None (verification + superrepo gitlink commit).

- [ ] **Step 1: Run the full automated suites**

Run: `cd PebbleCountdownTimer && gcc -I src/c tests/test_timer_calc.c src/c/timer_calc.c -o /tmp/t && /tmp/t && npm test 2>&1 | tail -8`
Expected: `All timer_calc tests passed` and all PKJS tests passing.

- [ ] **Step 2: Boot the emulator (diorite — fast, reliable)**

Run: `cd /home/dev/pebble-timetracking && scripts/pebble-emu-boot.sh diorite`
Expected: boots within a couple of attempts; emulator stays up.

- [ ] **Step 3: Install and screenshot the list + a detail window**

Run (separate calls — do NOT `&&`-chain install+screenshot):
```bash
cd PebbleCountdownTimer
PEBBLE_QEMU_PATH=~/.local/bin/qemu-pebble-headless pebble install --emulator diorite
PEBBLE_QEMU_PATH=~/.local/bin/qemu-pebble-headless pebble screenshot --no-open /tmp/cdt-list.png
```
Manually verify on the device (push buttons via `pebble emu-*` or inspect):
- Detail of a RUNNING timer shows **Stop** on top, then Pause, +1, -1, Delete.
- An idle timer's detail shows Start, +1, -1, Delete (no "Start & Save"); after one
  `+1 min`, "Start & Save" appears at row 1 and the cursor stays on `+1 min`.
- Delete opens the "Delete?" confirm; Back returns to detail; Select removes the
  row and returns to the list.

Surface every screenshot to the user via SendUserFile.

- [ ] **Step 4: Commit the submodule gitlink in the superrepo**

```bash
cd /home/dev/pebble-timetracking
git add PebbleCountdownTimer
git commit -m "Bump PebbleCountdownTimer: detail-menu actions (Stop default, no-dup save, Delete, exit reason)"
```

- [ ] **Step 5: Confirm the submodule is on master (not detached)**

Run: `git -C PebbleCountdownTimer branch --show-current`
Expected: `master`. If empty (detached), run `git -C PebbleCountdownTimer checkout master && git -C PebbleCountdownTimer merge --ff-only HEAD@{1}` is NOT correct — instead `git -C PebbleCountdownTimer checkout -B master <sha>` where `<sha>` is the latest submodule commit, then re-bump the gitlink.

---

## Self-review notes

- **Spec coverage:** Req 1 (no dup) → Task 1 `tc_detail_actions` (Save row only when changed) + Task 3 dispatch. Req 2 (delete) → Task 4 (phone) + Task 5 (watch). Req 3 (exit reason) → Task 2. Req 4 (Stop default) → Task 1 ordering + Task 3 wiring. Testing section → Tasks 1, 4, 6.
- **Cursor-shift footgun** (idle `+1` introduces the Save row, shifting `+1` down): handled by `dl_select_action(a)` in Task 3 Step 4, which re-homes the cursor onto the pressed +/- button.
- **Build-order caveat:** Task 3 leaves `open_delete_confirm` undefined until Task 5; the only acceptable error from the Task 3 build is the missing-reference link error. The first fully-successful build is Task 5 Step 4 (after `pebble clean`).
