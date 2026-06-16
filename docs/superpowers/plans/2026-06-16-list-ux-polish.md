# PebbleCountdownTimer List UX Polish Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a brief "started" confirmation screen, state-colored list rows, and an optional "running timers first" ordering to the PebbleCountdownTimer watchapp.

**Architecture:** Pure-logic sort change in `timer_calc` (unit-tested with host gcc), persisted config flag mirrored watch↔phone (`RunningFirst`), and two presentation-only changes in `main.c` (row tint in `ml_draw_row`, a transient confirmation window before auto-return). Phone side is TypeScript compiled to `src/pkjs`.

**Tech Stack:** C (Pebble SDK), TypeScript (`src/ts` → `src/pkjs`), Clay config page, node:test for JS, plain gcc for the `timer_calc` C test.

---

## Working directory & baseline commands

All paths are relative to `/home/dev/pebble-timetracking/PebbleCountdownTimer` unless noted. The submodule is on branch `master`.

- **C unit test** (no SDK needed):
  `gcc -I src/c tests/test_timer_calc.c src/c/timer_calc.c -o /tmp/tct && /tmp/tct`
  Passing prints `All timer_calc tests passed`.
- **JS tests** (regenerates `src/pkjs` via `tsc` first): `npm test`
  (`node_modules` is already installed; if missing, run `npm install` once. Needs `gie net loose` only if `node_modules` is absent.)
- **Watchapp build** (after touching `messageKeys`, run `pebble clean` first):
  `PEBBLE_QEMU_PATH=~/.local/bin/qemu-pebble-headless pebble clean && PEBBLE_QEMU_PATH=~/.local/bin/qemu-pebble-headless pebble build`

**NOTE:** `scripts/check-pebble-message-keys.py` does NOT cover PebbleCountdownTimer (only TimeStyle + TwtControl), and there is no Android side for this app. So the only `messageKeys` discipline here is: **append the new key at the END**, then `pebble clean && pebble build` to regenerate the `MESSAGE_KEY_*` C macros.

---

## File Structure

- `src/c/timer_calc.h` / `src/c/timer_calc.c` — `tc_display_order` gains a `bool running_first` parameter (running timers float above non-running while intra-group order is preserved).
- `tests/test_timer_calc.c` — new assertions for running-first on/off; existing `tc_display_order` calls updated for the new signature.
- `src/c/timer_store.h` / `src/c/timer_store.c` — `RunningFirst` persistence (`PERSIST_KEY_RUNNINGFIRST 6`, default true).
- `src/ts/config_clay.ts` — new `RunningFirst` toggle (default true).
- `src/ts/index.ts` — send + persist `RunningFirst` on save.
- `src/ts/config_sync.ts` — include `RunningFirst` in the launch-time resend (default 1 when unset).
- `tests/config_sync.test.js` — assertions for `RunningFirst` in the resend dict.
- `package.json` — append `"RunningFirst"` to `messageKeys`.
- `src/c/main.c` — load/inbox/`rebuild_order` wiring for `s_running_first`; row tint in `ml_draw_row`; the start-confirmation window + its trigger in `ml_select`.

---

## Task 1: `tc_display_order` running-first ordering (pure logic, TDD)

**Files:**
- Modify: `src/c/timer_calc.h` (signature of `tc_display_order`)
- Modify: `src/c/timer_calc.c` (`order_key`, `tc_display_order`)
- Test: `tests/test_timer_calc.c`

- [ ] **Step 1: Update existing test calls to the new signature, then add the failing test**

In `tests/test_timer_calc.c`, the three existing `tc_display_order(...)` calls (the SORT_MRU `d` block and the SORT_SHORTEST/SORT_LONGEST `e` block) must get a trailing `false` argument so they keep asserting current behavior:

```c
  tc_display_order(d, 3, SORT_MRU, 0, order, false);
  assert(order[0] == 2 && order[1] == 0 && order[2] == 1);
```
```c
  tc_display_order(e, 3, SORT_SHORTEST, 1000, order, false);
  assert(order[0] == 1 && order[1] == 2 && order[2] == 0);  // 50, 120, 300
  tc_display_order(e, 3, SORT_LONGEST, 1000, order, false);
  assert(order[0] == 0 && order[1] == 2 && order[2] == 1);  // 300, 120, 50
```

Then add this new block immediately after the SORT_LONGEST assertion (line ~79):

```c
  // --- display_order: running_first floats RUNNING above non-running ---
  Timer rf[4]; memset(rf, 0, sizeof(rf));
  rf[0].state = TS_IDLE;    rf[0].last_used = 100;                         // non-running
  rf[1].state = TS_RUNNING; rf[1].end_time = 5000; rf[1].last_used = 10;   // running
  rf[2].state = TS_PAUSED;  rf[2].last_used = 200;                         // non-running
  rf[3].state = TS_RUNNING; rf[3].end_time = 6000; rf[3].last_used = 50;   // running
  int rford[4];
  // ON, MRU: running group first by last_used desc (3,1), then non-running (2,0)
  tc_display_order(rf, 4, SORT_MRU, 0, rford, true);
  assert(rford[0] == 3 && rford[1] == 1 && rford[2] == 2 && rford[3] == 0);
  // OFF: pure MRU desc -> 2(200), 0(100), 3(50), 1(10)
  tc_display_order(rf, 4, SORT_MRU, 0, rford, false);
  assert(rford[0] == 2 && rford[1] == 0 && rford[2] == 3 && rford[3] == 1);
```

- [ ] **Step 2: Run the C test to verify it fails to compile**

Run: `gcc -I src/c tests/test_timer_calc.c src/c/timer_calc.c -o /tmp/tct`
Expected: compile error — too many arguments to `tc_display_order` (signature not yet updated).

- [ ] **Step 3: Update the header signature**

In `src/c/timer_calc.h`, change the declaration (around line 40) to:

```c
// Fill order[count] with timer indices sorted per `mode` (SORT_MRU: last_used
// desc; SORT_SHORTEST/LONGEST: remaining-at-`now` asc/desc), ties by index asc.
// When running_first is true, RUNNING timers sort above all non-running ones,
// preserving each group's intra-order.
void tc_display_order(const Timer *t, int count, SortMode mode, int64_t now, int *order, bool running_first);
```

- [ ] **Step 4: Update the implementation**

In `src/c/timer_calc.c`, change `order_key` and `tc_display_order`. The bias `1LL << 40` (~1.1e12) dominates `last_used` (epoch secs < 2^31) and any `remaining` (< 2^31), so running timers always sort above non-running while their relative order is unchanged:

```c
// Comparison key for a timer under `mode`. Returns a 64-bit value; the sort puts
// HIGHER keys first, so we negate where the mode wants ascending order. When
// running_first is set, RUNNING timers get a large bias so they sort above all
// non-running timers regardless of mode.
static int64_t order_key(const Timer *t, SortMode mode, int64_t now, bool running_first) {
  int64_t base;
  if (mode == SORT_SHORTEST) { base = -(int64_t)tc_remaining_now(t, now); } // asc -> negate
  else if (mode == SORT_LONGEST)  { base =  (int64_t)tc_remaining_now(t, now); } // desc
  else { base = t->last_used; }                                              // MRU: desc
  if (running_first && t->state == TS_RUNNING) { base += (1LL << 40); }
  return base;
}

void tc_display_order(const Timer *t, int count, SortMode mode, int64_t now, int *order, bool running_first) {
  for (int i = 0; i < count; i++) { order[i] = i; }
  // stable insertion sort: higher order_key first, ties keep ascending index
  for (int i = 1; i < count; i++) {
    int key = order[i];
    int64_t kv = order_key(&t[key], mode, now, running_first);
    int j = i - 1;
    while (j >= 0 && order_key(&t[order[j]], mode, now, running_first) < kv) {
      order[j + 1] = order[j];
      j--;
    }
    order[j + 1] = key;
  }
}
```

- [ ] **Step 5: Run the C test to verify it passes**

Run: `gcc -I src/c tests/test_timer_calc.c src/c/timer_calc.c -o /tmp/tct && /tmp/tct`
Expected: `All timer_calc tests passed`

- [ ] **Step 6: Commit**

```bash
git add src/c/timer_calc.h src/c/timer_calc.c tests/test_timer_calc.c
git commit -m "timer_calc: running-first ordering option in tc_display_order"
```

---

## Task 2: `RunningFirst` persistence + phone config (TDD for JS resend)

**Files:**
- Modify: `src/c/timer_store.h`, `src/c/timer_store.c`
- Modify: `package.json`
- Modify: `src/ts/config_clay.ts`, `src/ts/index.ts`, `src/ts/config_sync.ts`
- Test: `tests/config_sync.test.js`

- [ ] **Step 1: Add the failing JS test for the resend dict**

In `tests/config_sync.test.js`, update the two existing `deepStrictEqual` expectations to include `RunningFirst`, and add two new tests. The saved-config test becomes:

```js
test('resendDict: saved config -> dict with parsed SortOrder + AutoReturn + RunningFirst', () => {
  assert.deepStrictEqual(
    resendDict(store({ timer_config: 'Egg\x1f300\x1eTea\x1f120', sort_order: '1', auto_return: '1', running_first: '1' })),
    { TimerConfig: 'Egg\x1f300\x1eTea\x1f120', SortOrder: 1, AutoReturn: 1, RunningFirst: 1 });
});
```

The explicitly-empty-list test becomes (running_first absent -> defaults to 1, ON):

```js
test('resendDict: explicitly-saved empty list ("") IS sent (user cleared all timers)', () => {
  assert.deepStrictEqual(
    resendDict(store({ timer_config: '', sort_order: '0' })),
    { TimerConfig: '', SortOrder: 0, AutoReturn: 0, RunningFirst: 1 });
});
```

Add two new tests:

```js
test('resendDict: missing running_first defaults to 1 (ON) for pre-feature saves', () => {
  assert.strictEqual(resendDict(store({ timer_config: 'a\x1f60' })).RunningFirst, 1);
});

test('resendDict: saved running_first "0" round-trips to 0', () => {
  assert.strictEqual(resendDict(store({ timer_config: 'a\x1f60', running_first: '0' })).RunningFirst, 0);
});
```

- [ ] **Step 2: Run the JS tests to verify they fail**

Run: `npm test`
Expected: the config_sync suite fails (`RunningFirst` missing / undefined).

- [ ] **Step 3: Implement `RunningFirst` in the resend dict**

In `src/ts/config_sync.ts`, change the returned object so `RunningFirst` defaults to 1 (ON) when never saved — a `null` from `get` means a pre-feature save, which must NOT clobber the watch's default-on:

```ts
export function resendDict(get: (k: string) => string | null): Record<string, any> | null {
  const tc = get('timer_config');
  if (tc === null) { return null; }
  const rf = get('running_first');
  return {
    TimerConfig: tc,
    SortOrder: parseInt(get('sort_order') || '0', 10) || 0,
    AutoReturn: parseInt(get('auto_return') || '0', 10) || 0,
    RunningFirst: rf === null ? 1 : (parseInt(rf, 10) ? 1 : 0),
  };
}
```

- [ ] **Step 4: Run the JS tests to verify they pass**

Run: `npm test`
Expected: all suites pass (config_sync includes the new assertions).

- [ ] **Step 5: Add the Clay toggle**

In `src/ts/config_clay.ts`, inside the Display `section` `items` array, add a toggle after the `AutoReturn` toggle (line ~30):

```ts
      { type: 'toggle', messageKey: 'RunningFirst',
        label: 'Show running timers at the top', defaultValue: true },
```

- [ ] **Step 6: Send + persist `RunningFirst` on save**

In `src/ts/index.ts`, inside the `webviewclosed` handler, after the `dict.AutoReturn` line add:

```ts
  dict.RunningFirst = s.RunningFirst ? 1 : 0;
```

and after the `window.localStorage.setItem('auto_return', ...)` line add:

```ts
  window.localStorage.setItem('running_first', String(dict.RunningFirst));
```

- [ ] **Step 7: Append the message key**

In `package.json`, append `"RunningFirst"` to the END of `messageKeys`:

```json
    "messageKeys": ["TimerConfig", "SortOrder", "Request", "AutoReturn", "RunningFirst"],
```

- [ ] **Step 8: Add C persistence**

In `src/c/timer_store.h`, after `#define PERSIST_KEY_AUTORETURN 5` add:

```c
#define PERSIST_KEY_RUNNINGFIRST 6
```

and after the `store_save_autoreturn` declaration add:

```c
// Running-timers-first list ordering (defaults to true when unset).
bool store_load_runningfirst(void);
void store_save_runningfirst(bool on);
```

In `src/c/timer_store.c`, after the `store_save_autoreturn` definition add:

```c
bool store_load_runningfirst(void) {
  if (!persist_exists(PERSIST_KEY_RUNNINGFIRST)) { return true; }   // default ON
  return persist_read_bool(PERSIST_KEY_RUNNINGFIRST);
}

void store_save_runningfirst(bool on) {
  persist_write_bool(PERSIST_KEY_RUNNINGFIRST, on);
}
```

- [ ] **Step 9: Regenerate macros and build**

Run: `PEBBLE_QEMU_PATH=~/.local/bin/qemu-pebble-headless pebble clean && PEBBLE_QEMU_PATH=~/.local/bin/qemu-pebble-headless pebble build`
Expected: build succeeds (the `MESSAGE_KEY_RunningFirst` macro is regenerated; note `main.c` doesn't use it yet — that's Task 3, so this build just confirms config/key compile).

- [ ] **Step 10: Commit**

```bash
git add src/c/timer_store.h src/c/timer_store.c package.json src/ts/config_clay.ts src/ts/index.ts src/ts/config_sync.ts tests/config_sync.test.js
git commit -m "Add RunningFirst config flag (Clay toggle + persistence + resend)"
```

---

## Task 3: Wire `s_running_first` into the watch list ordering

**Files:**
- Modify: `src/c/main.c`

- [ ] **Step 1: Add the state variable**

In `src/c/main.c`, after `static bool s_auto_return = false;` (line ~34) add:

```c
static bool s_running_first = true; // config: float RUNNING timers to the top
```

- [ ] **Step 2: Pass it into the sort**

Change `rebuild_order` (line ~64) to:

```c
static void rebuild_order(void) { tc_display_order(s_timers, s_count, s_sort, now_s(), s_order, s_running_first); }
```

- [ ] **Step 3: Load it at init**

In `init()`, after `s_auto_return = store_load_autoreturn();` (line ~507) add:

```c
  s_running_first = store_load_runningfirst();
```

- [ ] **Step 4: Read it from the inbox**

In `inbox_received`, after the `autoret` block (line ~450, before the `cfg` block) add:

```c
  Tuple *runfirst = dict_find(iter, MESSAGE_KEY_RunningFirst);
  if (runfirst) {
    s_running_first = runfirst->value->int32 != 0;
    store_save_runningfirst(s_running_first);
  }
```

- [ ] **Step 5: Build to verify it compiles**

Run: `PEBBLE_QEMU_PATH=~/.local/bin/qemu-pebble-headless pebble build`
Expected: build succeeds.

- [ ] **Step 6: Commit**

```bash
git add src/c/main.c
git commit -m "Watch: apply RunningFirst to list ordering"
```

---

## Task 4: State-colored list rows

**Files:**
- Modify: `src/c/main.c` (`ml_draw_row`, line ~406)

- [ ] **Step 1: Replace `ml_draw_row` with the tinted version**

Replace the body of `ml_draw_row` (lines ~406-421) with:

```c
static void ml_draw_row(GContext *gctx, const Layer *cell, MenuIndex *ci, void *ctx) {
  if (s_count == 0) {
    menu_cell_basic_draw(gctx, cell, "No timers", "Configure on your phone", NULL);
    return;
  }
  int idx = s_order[ci->row];
  Timer *t = &s_timers[idx];
  // Tint non-selected rows by state (color displays only). The selected row keeps
  // MenuLayer's standard highlight (black bg / white text) so the cursor is obvious.
  MenuIndex sel = menu_layer_get_selected_index(s_menu);
  if (menu_index_compare(&sel, ci) != 0) {
    GColor bg = GColorWhite;
    switch (t->state) {
      case TS_RUNNING: bg = PBL_IF_COLOR_ELSE(GColorMintGreen, GColorWhite); break;
      case TS_PAUSED:  bg = PBL_IF_COLOR_ELSE(GColorPastelYellow, GColorWhite); break;
      case TS_DONE:    bg = PBL_IF_COLOR_ELSE(GColorMelon, GColorWhite); break;
      default:         break;   // TS_IDLE -> white
    }
    graphics_context_set_fill_color(gctx, bg);
    graphics_fill_rect(gctx, layer_get_bounds(cell), 0, GCornerNone);
    graphics_context_set_text_color(gctx, GColorBlack);
  }
  char rem[16]; tc_format_remaining(rem, sizeof(rem), tc_remaining_now(t, now_s()));
  if (t->name[0]) {
    char sub[40]; snprintf(sub, sizeof(sub), "%s  %s", rem, state_label(t->state));
    menu_cell_basic_draw(gctx, cell, t->name, sub, NULL);
  } else {
    menu_cell_basic_draw(gctx, cell, rem, state_label(t->state), NULL);
  }
}
```

- [ ] **Step 2: Build to verify it compiles**

Run: `PEBBLE_QEMU_PATH=~/.local/bin/qemu-pebble-headless pebble build`
Expected: build succeeds.

- [ ] **Step 3: Commit**

```bash
git add src/c/main.c
git commit -m "Watch: tint list rows by timer state (running/paused/done)"
```

(Visual verification is in Task 6 via emery screenshot.)

---

## Task 5: Start confirmation window (auto-return path only)

**Files:**
- Modify: `src/c/main.c`

- [ ] **Step 1: Add confirmation window state + helpers**

In `src/c/main.c`, near the other window statics (after `s_detail_*`, line ~39) add:

```c
// ---- transient "Started" confirmation shown ~1.1s before auto-return closes the app ----
static Window  *s_confirm_window;
static Layer   *s_confirm_layer;
static AppTimer *s_confirm_timer;
static char s_confirm_name[NAME_LEN + 1];
static char s_confirm_time[16];
static bool s_confirm_named;
```

Add these functions just above `ml_select` (line ~423). The checkmark is drawn with two thick lines (a system-font glyph for ✓ is not guaranteed):

```c
static void confirm_timer_cb(void *data) {
  s_confirm_timer = NULL;
  window_stack_pop_all(true);   // close the app -> watchface
}

static void confirm_update_proc(Layer *layer, GContext *gctx) {
  GRect b = layer_get_bounds(layer);
  int cx = b.size.w / 2;
  int cy = b.size.h / 2;
  // checkmark (two thick strokes), black on the green/white window background
  graphics_context_set_stroke_color(gctx, GColorBlack);
  graphics_context_set_stroke_width(gctx, 4);
  graphics_draw_line(gctx, GPoint(cx - 18, cy - 34), GPoint(cx - 6, cy - 22));
  graphics_draw_line(gctx, GPoint(cx - 6, cy - 22), GPoint(cx + 20, cy - 48));
  graphics_context_set_text_color(gctx, GColorBlack);
  // name (or the time, if unnamed)
  graphics_draw_text(gctx, s_confirm_name, fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD),
    GRect(4, cy - 14, b.size.w - 8, 30), GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
  // duration (only when named, to avoid showing the time twice)
  if (s_confirm_named) {
    graphics_draw_text(gctx, s_confirm_time, fonts_get_system_font(FONT_KEY_GOTHIC_18),
      GRect(4, cy + 16, b.size.w - 8, 22), GTextOverflowModeFill, GTextAlignmentCenter, NULL);
  }
  // "Started"
  graphics_draw_text(gctx, "Started", fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
    GRect(4, cy + 38, b.size.w - 8, 22), GTextOverflowModeFill, GTextAlignmentCenter, NULL);
}

static void confirm_window_load(Window *w) {
  Layer *root = window_get_root_layer(w);
  s_confirm_layer = layer_create(layer_get_bounds(root));
  layer_set_update_proc(s_confirm_layer, confirm_update_proc);
  layer_add_child(root, s_confirm_layer);
}

static void confirm_window_unload(Window *w) {
  if (s_confirm_timer) { app_timer_cancel(s_confirm_timer); s_confirm_timer = NULL; }
  if (s_confirm_layer) { layer_destroy(s_confirm_layer); s_confirm_layer = NULL; }
}

// Flash a "Started" screen for ~1.1s, then pop the whole stack (-> watchface).
// Only called on the idle one-tap start when AutoReturn is on.
static void show_start_confirmation(int idx) {
  Timer *t = &s_timers[idx];
  tc_format_remaining(s_confirm_time, sizeof(s_confirm_time), tc_remaining_now(t, now_s()));
  s_confirm_named = (t->name[0] != 0);
  if (s_confirm_named) {
    strncpy(s_confirm_name, t->name, sizeof(s_confirm_name));
  } else {
    strncpy(s_confirm_name, s_confirm_time, sizeof(s_confirm_name));
  }
  s_confirm_name[sizeof(s_confirm_name) - 1] = 0;
  if (!s_confirm_window) {
    s_confirm_window = window_create();
    window_set_background_color(s_confirm_window, PBL_IF_COLOR_ELSE(GColorMintGreen, GColorWhite));
    window_set_window_handlers(s_confirm_window,
      (WindowHandlers){ .load = confirm_window_load, .unload = confirm_window_unload });
  }
  window_stack_push(s_confirm_window, true);
  s_confirm_timer = app_timer_register(1100, confirm_timer_cb, NULL);
}
```

- [ ] **Step 2: Trigger it from the idle one-tap start**

In `ml_select` (line ~423), replace the idle branch's `return_to_watchface();` call so the confirmation shows when auto-return is on:

```c
  if (s_timers[idx].state == TS_IDLE) {
    tc_start(&s_timers[idx], now_s());
    persist_all(); rearm_wakeup(); ensure_ticking(); reload_ui();
    select_timer_row(idx);
    if (s_auto_return) { show_start_confirmation(idx); }   // flash, then pop to watchface
    return;
  }
```

(`return_to_watchface()` is now unused by this path. Leave the function in place — it documents the auto-return semantics and may be referenced elsewhere; if the build warns it is unused, delete the function definition at line ~273.)

- [ ] **Step 3: Destroy the confirmation window on deinit**

In `deinit()` (line ~541), before `window_destroy(s_window);` add:

```c
  if (s_confirm_window) { window_destroy(s_confirm_window); }
```

- [ ] **Step 4: Build to verify it compiles**

Run: `PEBBLE_QEMU_PATH=~/.local/bin/qemu-pebble-headless pebble build`
Expected: build succeeds (if `return_to_watchface` triggers an unused-function warning that fails the build, delete its definition and re-build).

- [ ] **Step 5: Commit**

```bash
git add src/c/main.c
git commit -m "Watch: show 'Started' confirmation before auto-return closes the app"
```

---

## Task 6: Emulator screenshot verification + submodule/superrepo finalize

**Files:**
- Verify only; then superrepo gitlink bump.

- [ ] **Step 1: Boot the emery emulator**

Run: `scripts/pebble-emu-boot.sh emery` (from the repo root `/home/dev/pebble-timetracking`).
Expected: boots within a couple of attempts; thereafter the emulator stays up.

- [ ] **Step 2: Build with screenshot fixtures and install**

The fixtures (`#ifdef SCREENSHOT_FIXTURES` in `main.c:508`) seed an `Egg` (RUNNING), `Tea` (PAUSED) and `Laundry` (DONE) timer — exactly the three tinted states.

Run (in `PebbleCountdownTimer`):
```bash
SCREENSHOT_FIXTURES=1 PEBBLE_QEMU_PATH=~/.local/bin/qemu-pebble-headless pebble clean
SCREENSHOT_FIXTURES=1 PEBBLE_QEMU_PATH=~/.local/bin/qemu-pebble-headless pebble build
PEBBLE_QEMU_PATH=~/.local/bin/qemu-pebble-headless pebble install --emulator emery
```
Expected: `App install succeeded`.

- [ ] **Step 3: Screenshot and surface to the user**

Run (separate call): `PEBBLE_QEMU_PATH=~/.local/bin/qemu-pebble-headless pebble screenshot --no-open /tmp/cdt-rows.png`
Then surface `/tmp/cdt-rows.png` to the user with `SendUserFile`. Confirm: the RUNNING row reads with a dim green background, PAUSED dim yellow, DONE dim red; the currently-selected row shows the standard highlight (not tinted). With RunningFirst default on, `Egg` (RUNNING) is at the top.

- [ ] **Step 4: Rebuild WITHOUT fixtures (ship a clean bundle)**

Run:
```bash
PEBBLE_QEMU_PATH=~/.local/bin/qemu-pebble-headless pebble clean
PEBBLE_QEMU_PATH=~/.local/bin/qemu-pebble-headless pebble build
```
Expected: build succeeds; `build/*.pbw` produced without seeded fixtures.

- [ ] **Step 5: Final test sweep**

Run: `gcc -I src/c tests/test_timer_calc.c src/c/timer_calc.c -o /tmp/tct && /tmp/tct && npm test`
Expected: `All timer_calc tests passed` and all JS suites pass.

- [ ] **Step 6: Bump the superrepo gitlink**

The submodule commits are local-only until the gitlink is bumped. From `/home/dev/pebble-timetracking`:
```bash
git add PebbleCountdownTimer
git commit -m "Bump PebbleCountdownTimer: list UX polish (confirmation, row tints, running-first)"
```

(Pushing the submodule needs a YubiKey touch and is out of scope for the agent — leave it for the user unless asked.)

---

## Notes / gotchas carried from CLAUDE.md

- `strtol`/`atol`/`strtod` are NOT exported by the Core Devices firmware — this plan adds no such calls (all parsing stays on the phone JS side).
- Big stack locals overflow the ~2 KB app stack — the confirmation buffers are file-scope `static`, not stack locals.
- After editing `messageKeys`, the `MESSAGE_KEY_*` C macros are cached — always `pebble clean` once (Task 2 Step 9).
- Use `diorite` for a quick 144px check; `emery` (Pebble Time 2, color) is the real test board for the tints. Never `basalt` (headless crash-loop). Never `pkill -f` qemu — use `pebble kill` / `pkill -x qemu-pebble`.
