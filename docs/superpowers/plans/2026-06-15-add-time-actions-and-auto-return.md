# +N min actions & auto-return-to-watchface Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add `+1/+5/+10 min` action-menu items for running/paused countdown timers, and a config toggle that returns to the watchface after a timer is started.

**Architecture:** A new pure `tc_add()` in the host-testable core handles the time math; `main.c` wires it into a state-aware action menu via a single shared callback. The auto-return toggle follows the existing Clay-setting → AppMessage → persist path (like `SortOrder`), and `main.c` calls `window_stack_pop_all` after any start/resume when the flag is set.

**Tech Stack:** C (Pebble SDK), TypeScript (compiled to `src/pkjs/` by `tsc`), Clay config, node:test for JS, plain `gcc` for the C core.

---

## File Structure

- `src/c/timer_calc.h` / `src/c/timer_calc.c` — add `tc_add()` (pure, host-testable).
- `tests/test_timer_calc.c` — unit tests for `tc_add()`.
- `src/c/main.c` — state-aware action menu + `act_add_time` callback (feature 1);
  `s_auto_return` flag, inbox handling, `return_to_watchface()` (feature 2).
- `src/c/timer_store.h` / `src/c/timer_store.c` — persist the auto-return flag.
- `src/ts/config_clay.ts` — add the `AutoReturn` toggle.
- `package.json` — append `"AutoReturn"` message key.
- `src/ts/index.ts` — send + persist `AutoReturn` on save.
- `src/ts/config_sync.ts` — include `AutoReturn` in the launch-handshake reply.
- `tests/config_sync.test.js` — cover `AutoReturn` in `resendDict`.

Feature 1 (Tasks 1-2) and Feature 2 (Tasks 3-6) are independent; Feature 1 is
self-contained C work, so do it first.

---

## Task 1: `tc_add()` core function

**Files:**
- Modify: `src/c/timer_calc.h` (declare after `tc_extend`, ~line 49)
- Modify: `src/c/timer_calc.c` (implement after `tc_extend`, ~line 133)
- Test: `tests/test_timer_calc.c`

- [ ] **Step 1: Write the failing test**

Add these blocks to `tests/test_timer_calc.c` immediately before the final
`printf("All timer_calc tests passed\n");` line:

```c
  // --- tc_add: running extends end_time; paused grows remaining; stamps last_used ---
  Timer a; memset(&a, 0, sizeof(a)); a.duration = 300;
  tc_start(&a, 1000);                 // RUNNING, end_time = 1300
  tc_add(&a, 300, 1100);             // +5 min while running
  assert(a.state == TS_RUNNING && a.end_time == 1600 && a.last_used == 1100);
  assert(tc_remaining_now(&a, 1100) == 500);

  Timer p; memset(&p, 0, sizeof(p)); p.duration = 600; p.state = TS_PAUSED; p.remaining = 120;
  tc_add(&p, 60, 2000);             // +1 min while paused
  assert(p.state == TS_PAUSED && p.remaining == 180 && p.last_used == 2000);

  // idle / done: no-op (state and timing unchanged)
  Timer id; memset(&id, 0, sizeof(id)); id.duration = 90; id.state = TS_IDLE; id.remaining = 90;
  tc_add(&id, 600, 3000);
  assert(id.state == TS_IDLE && id.remaining == 90 && id.last_used == 0);
  Timer dn; memset(&dn, 0, sizeof(dn)); dn.state = TS_DONE; dn.remaining = 0;
  tc_add(&dn, 600, 3000);
  assert(dn.state == TS_DONE && dn.remaining == 0 && dn.last_used == 0);
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd PebbleCountdownTimer && gcc -I src/c tests/test_timer_calc.c src/c/timer_calc.c -o /tmp/tc_test`
Expected: FAIL to compile — `implicit declaration of function 'tc_add'`.

- [ ] **Step 3: Declare `tc_add` in the header**

In `src/c/timer_calc.h`, add after the `tc_extend` declaration (after line 49):

```c
// Add `secs` to the time left. RUNNING: end_time += secs (extends the live
// countdown). PAUSED: remaining += secs (clamped >= 0). IDLE/DONE: no-op. Stamps
// last_used = now. Distinct from tc_extend, which SETS end_time and forces RUNNING.
void tc_add(Timer *t, int32_t secs, int64_t now);
```

- [ ] **Step 4: Implement `tc_add`**

In `src/c/timer_calc.c`, add after the `tc_extend` function (after line 133):

```c
void tc_add(Timer *t, int32_t secs, int64_t now) {
  if (t->state == TS_RUNNING) {
    t->end_time += secs;
  } else if (t->state == TS_PAUSED) {
    t->remaining += secs;
    if (t->remaining < 0) { t->remaining = 0; }
  } else {
    return;   // IDLE/DONE: not applicable
  }
  t->last_used = now;
}
```

- [ ] **Step 5: Run test to verify it passes**

Run: `cd PebbleCountdownTimer && gcc -I src/c tests/test_timer_calc.c src/c/timer_calc.c -o /tmp/tc_test && /tmp/tc_test`
Expected: PASS — `All timer_calc tests passed`.

- [ ] **Step 6: Commit**

```bash
cd PebbleCountdownTimer
git add src/c/timer_calc.h src/c/timer_calc.c tests/test_timer_calc.c
git commit -m "Core: tc_add() to grow a running/paused timer's time left"
```

---

## Task 2: `+1/+5/+10 min` action-menu items

**Files:**
- Modify: `src/c/main.c` (add `act_add_time` near `act_reset` ~line 268; rewrite
  `open_action_menu` ~lines 276-290)

No host unit test — this is on-watch UI wiring (same rationale as the alarm screen;
verified via build + emulator screenshots). The `tc_add` logic it calls is covered
by Task 1.

- [ ] **Step 1: Add the shared `act_add_time` callback**

In `src/c/main.c`, add immediately after `act_reset` (after line 268, before
`action_menu_did_close`):

```c
static void act_add_time(ActionMenu *am, const ActionMenuItem *item, void *ctx) {
  int idx = (int)(intptr_t)action_menu_get_context(am);
  if (idx < 0 || idx >= s_count) { return; }
  int32_t secs = (int32_t)(intptr_t)action_menu_item_get_action_data(item);
  tc_add(&s_timers[idx], secs, now_s());
  persist_all(); rearm_wakeup(); ensure_ticking(); reload_ui();
  select_timer_row(idx);
}
```

- [ ] **Step 2: Make `open_action_menu` state-aware**

In `src/c/main.c`, replace the whole `open_action_menu` function (lines 276-290)
with:

```c
static void open_action_menu(int timer_idx) {
  Timer *t = &s_timers[timer_idx];
  bool addable = (t->state == TS_RUNNING || t->state == TS_PAUSED);
  int count = addable ? 5 : 2;
  s_action_root = action_menu_level_create(count);
  action_menu_level_add_action(s_action_root,
    (t->state == TS_RUNNING) ? "Pause" : "Start", act_toggle, NULL);
  if (addable) {
    action_menu_level_add_action(s_action_root, "+1 min",  act_add_time, (void *)(intptr_t)60);
    action_menu_level_add_action(s_action_root, "+5 min",  act_add_time, (void *)(intptr_t)300);
    action_menu_level_add_action(s_action_root, "+10 min", act_add_time, (void *)(intptr_t)600);
  }
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
```

- [ ] **Step 3: Build for the watch**

Run: `cd PebbleCountdownTimer && PEBBLE_QEMU_PATH=~/.local/bin/qemu-pebble-headless pebble build`
Expected: build succeeds (`'BUILD SUCCESSFUL'` / a fresh `build/*.pbw`).

- [ ] **Step 4: Screenshot the menu on emery (running + paused timer)**

Boot once, install, screenshot (CLAUDE.md headless recipe; the app ships
`SCREENSHOT_FIXTURES` seeding an Egg=running and Tea=paused timer):

```bash
cd /home/dev/pebble-timetracking
scripts/pebble-emu-boot.sh emery
cd PebbleCountdownTimer
PEBBLE_QEMU_PATH=~/.local/bin/qemu-pebble-headless pebble install   --emulator emery
# select the running "Egg" row to open its action menu, then:
PEBBLE_QEMU_PATH=~/.local/bin/qemu-pebble-headless pebble screenshot --no-open /tmp/menu-running.png
```

Expected: the action menu lists `Pause / +1 min / +5 min / +10 min / Reset`.
Surface the screenshot to the user via SendUserFile. (Driving the emulator's
buttons headless is limited — if the menu can't be opened via the serial console,
verify on the real watch in Task 6 instead and note that here.)

- [ ] **Step 5: Commit**

```bash
cd PebbleCountdownTimer
git add src/c/main.c
git commit -m "Watch: +1/+5/+10 min action items for running/paused timers"
```

---

## Task 3: persist the auto-return flag (store layer)

**Files:**
- Modify: `src/c/timer_store.h` (add key + two declarations)
- Modify: `src/c/timer_store.c` (add two functions)

- [ ] **Step 1: Add the persist key and declarations**

In `src/c/timer_store.h`, add the key alongside the others (after
`#define PERSIST_KEY_SORTORDER 4`):

```c
#define PERSIST_KEY_AUTORETURN 5
```

And add after the sort declarations (after `void store_save_sort(int mode);`):

```c
// Auto-return-to-watchface flag (defaults to false when unset).
bool store_load_autoreturn(void);
void store_save_autoreturn(bool on);
```

- [ ] **Step 2: Implement the two functions**

In `src/c/timer_store.c`, add at the end of the file:

```c
bool store_load_autoreturn(void) {
  if (!persist_exists(PERSIST_KEY_AUTORETURN)) { return false; }
  return persist_read_bool(PERSIST_KEY_AUTORETURN);
}

void store_save_autoreturn(bool on) {
  persist_write_bool(PERSIST_KEY_AUTORETURN, on);
}
```

- [ ] **Step 3: Verify it compiles (deferred to Task 4 build)**

No standalone build for the store layer (it needs the Pebble SDK, exercised by the
`pebble build` in Task 4). Just confirm the edits match the signatures used in Task 4.

- [ ] **Step 4: Commit**

```bash
cd PebbleCountdownTimer
git add src/c/timer_store.h src/c/timer_store.c
git commit -m "Store: persist auto-return-to-watchface flag (key 5)"
```

---

## Task 4: wire auto-return into `main.c`

**Files:**
- Modify: `src/c/main.c` (state var ~line 33; `inbox_received` ~lines 338-361;
  `ml_select` ~lines 324-335; `act_toggle` ~lines 252-260; `init` ~line 401)

- [ ] **Step 1: Add the state variable**

In `src/c/main.c`, add after `static int s_last_fired_idx = -1;` (line 33):

```c
static bool s_auto_return = false; // config: pop to watchface after a start/resume
```

- [ ] **Step 2: Add the `return_to_watchface` helper**

In `src/c/main.c`, add just before `open_action_menu` (before line 276):

```c
// Config option: leave the app (-> watchface) after a timer is started/resumed.
// The wakeup keeps a closed app's timer firing, so the alarm still triggers.
static void return_to_watchface(void) {
  if (s_auto_return) { window_stack_pop_all(true); }
}
```

- [ ] **Step 3: Apply it on the idle-Select start path**

In `src/c/main.c` `ml_select`, the `TS_IDLE` branch currently reads:

```c
  if (s_timers[idx].state == TS_IDLE) {
    tc_start(&s_timers[idx], now_s());
    persist_all(); rearm_wakeup(); ensure_ticking(); reload_ui();
    select_timer_row(idx);
  } else {
```

Add `return_to_watchface();` after `select_timer_row(idx);` inside that branch:

```c
  if (s_timers[idx].state == TS_IDLE) {
    tc_start(&s_timers[idx], now_s());
    persist_all(); rearm_wakeup(); ensure_ticking(); reload_ui();
    select_timer_row(idx);
    return_to_watchface();
  } else {
```

- [ ] **Step 4: Apply it on the menu Start/resume path**

In `src/c/main.c` `act_toggle`, after `select_timer_row(idx);` (end of the
function, line 259), add a RUNNING-only call:

```c
  persist_all(); rearm_wakeup(); ensure_ticking(); reload_ui();
  select_timer_row(idx);
  if (t->state == TS_RUNNING) { return_to_watchface(); }
}
```

(The existing first two lines already exist — only the final `if` line is new.)

- [ ] **Step 5: Read the flag from the inbox**

In `src/c/main.c` `inbox_received`, add after the `SortOrder` block (after line 345,
before the `TimerConfig` block):

```c
  Tuple *autoret = dict_find(iter, MESSAGE_KEY_AutoReturn);
  if (autoret) {
    s_auto_return = autoret->value->int32 != 0;
    store_save_autoreturn(s_auto_return);
  }
```

- [ ] **Step 6: Load the flag at init**

In `src/c/main.c` `init`, add after `s_sort = (SortMode)store_load_sort();` (line 401):

```c
  s_auto_return = store_load_autoreturn();
```

- [ ] **Step 7: Build (regenerate message-key macros first)**

`MESSAGE_KEY_AutoReturn` does not exist yet — it is added in Task 5. Do Task 5
before building. (If executing strictly in order, this step's build is performed at
the end of Task 5, Step 4.) No separate commit yet; commit after Task 5 so the
build is green.

- [ ] **Step 8: Commit**

```bash
cd PebbleCountdownTimer
git add src/c/main.c
git commit -m "Watch: return to watchface after start/resume when AutoReturn set"
```

---

## Task 5: Clay toggle + message key + phone JS

**Files:**
- Modify: `package.json` (line 18 `messageKeys`)
- Modify: `src/ts/config_clay.ts` (Display section ~lines 18-30)
- Modify: `src/ts/index.ts` (`webviewclosed` ~lines 37-40)
- Modify: `src/ts/config_sync.ts` (`resendDict` ~line 19)
- Test: `tests/config_sync.test.js`

- [ ] **Step 1: Write the failing test for `resendDict`**

In `tests/config_sync.test.js`, update the two existing positive-case assertions to
include `AutoReturn`, and add a new case. Replace the test
`'resendDict: saved config -> dict with parsed SortOrder'` body and add one after it:

```js
test('resendDict: saved config -> dict with parsed SortOrder + AutoReturn', () => {
  assert.deepStrictEqual(
    resendDict(store({ timer_config: 'Egg\x1f300\x1eTea\x1f120', sort_order: '1', auto_return: '1' })),
    { TimerConfig: 'Egg\x1f300\x1eTea\x1f120', SortOrder: 1, AutoReturn: 1 });
});

test('resendDict: missing auto_return defaults to 0', () => {
  assert.strictEqual(resendDict(store({ timer_config: 'a\x1f60' })).AutoReturn, 0);
});
```

Also update the empty-list case `'...empty list ("") IS sent...'` expectation to
include `AutoReturn: 0`:

```js
  assert.deepStrictEqual(
    resendDict(store({ timer_config: '', sort_order: '0' })),
    { TimerConfig: '', SortOrder: 0, AutoReturn: 0 });
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cd PebbleCountdownTimer && npm test`
Expected: FAIL — `resendDict` returns objects without an `AutoReturn` key.

- [ ] **Step 3: Add `AutoReturn` to `resendDict`**

In `src/ts/config_sync.ts`, change the return line to add `AutoReturn`:

```ts
  return {
    TimerConfig: tc,
    SortOrder: parseInt(get('sort_order') || '0', 10) || 0,
    AutoReturn: parseInt(get('auto_return') || '0', 10) || 0,
  };
```

- [ ] **Step 4: Run the test to verify it passes**

Run: `cd PebbleCountdownTimer && npm test`
Expected: PASS (the `pretest` `tsc` compiles `src/ts` → `src/pkjs` first).

- [ ] **Step 5: Add the Clay toggle**

In `src/ts/config_clay.ts`, add the toggle inside the Display section's `items`
array, after the `SortOrder` radiogroup (after line 28's closing `] }`):

```ts
      { type: 'toggle', messageKey: 'AutoReturn',
        label: 'Return to watchface after starting a timer', defaultValue: false },
```

- [ ] **Step 6: Send + persist `AutoReturn` on save**

In `src/ts/index.ts` `webviewclosed`, after the `dict.SortOrder = ...` line (line 37):

```ts
  dict.AutoReturn = s.AutoReturn ? 1 : 0;
```

and after the `sort_order` localStorage line (line 40):

```ts
  window.localStorage.setItem('auto_return', String(dict.AutoReturn));
```

- [ ] **Step 7: Append the message key**

In `package.json` line 18, append `"AutoReturn"`:

```json
    "messageKeys": ["TimerConfig", "SortOrder", "Request", "AutoReturn"],
```

- [ ] **Step 8: Clean build so the `MESSAGE_KEY_*` macros regenerate**

Run:
```bash
cd PebbleCountdownTimer
PEBBLE_QEMU_PATH=~/.local/bin/qemu-pebble-headless pebble clean
PEBBLE_QEMU_PATH=~/.local/bin/qemu-pebble-headless pebble build
```
Expected: build succeeds. (Without `clean`, the C side fails with
`'MESSAGE_KEY_AutoReturn' undeclared` — cached message-key header.)

- [ ] **Step 9: Run JS tests once more**

Run: `cd PebbleCountdownTimer && npm test`
Expected: PASS.

- [ ] **Step 10: Commit (JS + config + the Task 4 main.c build now green)**

```bash
cd PebbleCountdownTimer
git add package.json src/ts/config_clay.ts src/ts/index.ts src/ts/config_sync.ts \
        src/pkjs tests/config_sync.test.js
git commit -m "Config: AutoReturn toggle -> AppMessage + launch-handshake resend"
```

> NOTE: `src/pkjs/*.js` is gitignored (generated). The `git add src/pkjs` above is a
> no-op if so — leave it; it harmlessly stages nothing.

---

## Task 6: end-to-end verification on the real watch

**Files:** none (verification only).

- [ ] **Step 1: Full test sweep**

Run:
```bash
cd PebbleCountdownTimer
npm test
gcc -I src/c tests/test_timer_calc.c src/c/timer_calc.c -o /tmp/tc_test && /tmp/tc_test
```
Expected: JS tests PASS; `All timer_calc tests passed`.

- [ ] **Step 2: Install on the real Pebble (Core Devices)**

Run: `cd PebbleCountdownTimer && pebble install --cloudpebble build/*.pbw`
(Requires `pebble login` + same account on the phone; see CLAUDE.md.)

- [ ] **Step 3: Verify feature 1**

On the watch: start a timer, open its action menu → confirm `+1/+5/+10 min` appear;
tap `+5 min` and confirm the remaining time jumps up by 5:00. Pause the timer, open
the menu → confirm the items still appear and `+1 min` grows the paused remainder.

- [ ] **Step 4: Verify feature 2**

On the phone config page, enable "Return to watchface after starting a timer", Save.
On the watch: Select an idle timer → the app closes to the watchface. Reopen the
app, Start a paused timer via the menu → app closes to watchface. Wait for a short
timer to finish with the app closed → the full-screen alarm still fires.

- [ ] **Step 5: Update the spec's verification notes if anything differed**

If on-watch behaviour diverged from the spec (e.g. `window_stack_pop_all` timing
with the action menu still animating), note the actual behaviour in the spec doc
and commit:

```bash
cd PebbleCountdownTimer
git add docs/superpowers/specs/2026-06-15-add-time-actions-and-auto-return-design.md
git commit -m "Spec: record on-watch verification notes"
```

---

## Self-Review Notes

- **Spec coverage:** `tc_add` (Task 1) · flat menu items running+paused (Task 2) ·
  `AutoReturn` persist key (Task 3) · flag load/inbox/return-on-start (Task 4) ·
  Clay toggle + messageKey append + JS send/resend (Task 5) · verification (Task 6).
  All spec sections map to a task.
- **Cross-task type consistency:** `tc_add(Timer*, int32_t, int64_t)`,
  `store_load_autoreturn()/store_save_autoreturn(bool)`, `s_auto_return`,
  `return_to_watchface()`, `MESSAGE_KEY_AutoReturn`, localStorage key `auto_return`,
  dict key `AutoReturn` — used identically everywhere they appear.
- **Build-order caveat:** Task 4 references `MESSAGE_KEY_AutoReturn` (added in
  Task 5). When executing strictly in order, the first successful `pebble build` is
  Task 5 Step 8; commit Task 4's `main.c` before that build per its own Step 8, or
  reorder to land Task 5's `package.json` change first. Either way the final state
  builds clean.
