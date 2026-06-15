# Timer detail window Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the per-timer ActionMenu with a custom `MenuLayer` detail window that shows a live remaining-time header, white background / black text, compact rows ordered Pause/Reset/+1/+5/+10, and never auto-returns to the watchface.

**Architecture:** All changes are in `src/c/main.c`, mirroring the existing in-file alarm-window pattern. The ActionMenu and its callbacks are deleted; a new "detail window" section adds a `MenuLayer` with a header (`get_header_height`/`draw_header`) drawing the timer name + live time, state-dependent rows, and a single `select_click` handler. The existing 1 s `tick_cb` also reloads the detail menu so the header ticks. `tc_add` and the `AutoReturn` plumbing from the previous plan are untouched.

**Tech Stack:** C (Pebble SDK), `MenuLayer`, headless emulator screenshots.

---

## File Structure

- `src/c/main.c` — ONLY file changed. Removes the ActionMenu section
  (`s_action_root`, `act_toggle`, `act_reset`, `act_add_time`,
  `action_menu_did_close`, `open_action_menu`); keeps `select_timer_row` and
  `return_to_watchface`; adds the detail-window section + 3 globals; updates
  `tick_cb` and `ml_select`.

No host unit test: this is on-watch `MenuLayer` UI with no pure-logic surface (same
rationale as the alarm window). `tc_add`/`tc_start`/`tc_pause`/`tc_reset` are already
host-tested. Verification is build + emulator screenshots + real watch.

---

## Task 1: Replace the ActionMenu with the detail window

**Files:** Modify `src/c/main.c`.

- [ ] **Step 1: Add the detail-window globals**

In `src/c/main.c`, after the line `static bool s_auto_return = false; // config: pop to watchface after a start/resume` (currently line 34), add:

```c

// ---- per-timer detail window: live-time header + Pause/Reset/+N actions ----
static Window *s_detail_window;
static MenuLayer *s_detail_menu;
static int s_detail_idx = -1;   // config index the detail window is showing
```

(These are declared at the top so `tick_cb`, which appears before the detail-window
function definitions, can reference them.)

- [ ] **Step 2: Make `tick_cb` also refresh the detail header**

In `tick_cb`, the body currently is:

```c
  int fired = sweep_expiries();
  bool running = false;
  for (int i = 0; i < s_count; i++) { if (s_timers[i].state == TS_RUNNING) { running = true; } }
  reload_ui();
  if (fired) { persist_all(); rearm_wakeup(); trigger_alarm(s_last_fired_idx, fired); }
```

Insert the detail refresh right after `reload_ui();`:

```c
  reload_ui();
  if (s_detail_menu && window_stack_get_top_window() == s_detail_window) {
    menu_layer_reload_data(s_detail_menu);   // retick the live time header
  }
```

- [ ] **Step 3: Replace the entire ActionMenu section with the detail-window section**

Delete the whole block from the comment `// ---- ActionMenu: Start/Pause + Reset for the selected timer ----` (currently line 237) through the closing `}` of `open_action_menu` (currently line 316) — i.e. `s_action_root`, `select_timer_row`, `return_to_watchface`, `act_toggle`, `act_reset`, `act_add_time`, `action_menu_did_close`, and `open_action_menu`. Replace ALL of it with:

```c
// ---- per-timer detail window: live-time header + Pause/Reset/+N actions ----

// After acting on a timer it re-sorts (e.g. floats to the top in MRU mode); move the
// LIST cursor to follow it to its new row so the user needn't scroll to it.
static void select_timer_row(int idx) {
  if (!s_menu) { return; }
  for (int row = 0; row < s_count; row++) {
    if (s_order[row] == idx) {
      menu_layer_set_selected_index(s_menu, (MenuIndex){ .section = 0, .row = (uint16_t)row },
                                    MenuRowAlignTop, false);
      return;
    }
  }
}

// Config option: leave the app (-> watchface) after a timer is started. The wakeup
// keeps a closed app's timer firing, so the alarm still triggers. ONLY used by the
// idle one-tap start in ml_select; the detail window never auto-returns (the user may
// press +1 min several times).
static void return_to_watchface(void) {
  if (s_auto_return) { window_stack_pop_all(true); }
}

// true when the detail window's timer can take +N (it is running or paused).
static bool detail_addable(void) {
  if (s_detail_idx < 0 || s_detail_idx >= s_count) { return false; }
  TimerState st = s_timers[s_detail_idx].state;
  return st == TS_RUNNING || st == TS_PAUSED;
}

static uint16_t dl_num_rows(MenuLayer *ml, uint16_t section, void *ctx) {
  return detail_addable() ? 5 : 2;   // Pause/Start, Reset [, +1, +5, +10]
}
static int16_t dl_cell_height(MenuLayer *ml, MenuIndex *ci, void *ctx) { return 28; }
static int16_t dl_header_height(MenuLayer *ml, uint16_t section, void *ctx) { return 26; }

// Header: timer name (left) + live remaining time (right); time only if unnamed.
static void dl_draw_header(GContext *gctx, const Layer *cell, uint16_t section, void *ctx) {
  if (s_detail_idx < 0 || s_detail_idx >= s_count) { return; }
  Timer *t = &s_timers[s_detail_idx];
  char rem[16]; tc_format_remaining(rem, sizeof(rem), tc_remaining_now(t, now_s()));
  GRect b = layer_get_bounds(cell);
  GFont f = fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD);
  graphics_context_set_text_color(gctx, GColorBlack);
  if (t->name[0]) {
    graphics_draw_text(gctx, t->name, f, GRect(4, 2, b.size.w - 60, 22),
      GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
    graphics_draw_text(gctx, rem, f, GRect(4, 2, b.size.w - 8, 22),
      GTextOverflowModeTrailingEllipsis, GTextAlignmentRight, NULL);
  } else {
    graphics_draw_text(gctx, rem, f, GRect(4, 2, b.size.w - 8, 22),
      GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
  }
}

static const char *dl_row_label(int row, bool running) {
  switch (row) {
    case 0: return running ? "Pause" : "Start";
    case 1: return "Reset";
    case 2: return "+1 min";
    case 3: return "+5 min";
    case 4: return "+10 min";
    default: return "";
  }
}

static void dl_draw_row(GContext *gctx, const Layer *cell, MenuIndex *ci, void *ctx) {
  bool running = (s_detail_idx >= 0 && s_detail_idx < s_count
                  && s_timers[s_detail_idx].state == TS_RUNNING);
  GRect b = layer_get_bounds(cell);
  // MenuLayer pre-sets the text color (normal=black, highlight=white) and fills the
  // cell background before this callback, so just draw the label.
  graphics_draw_text(gctx, dl_row_label(ci->row, running),
    fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
    GRect(6, 1, b.size.w - 12, b.size.h - 1),
    GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
}

static void dl_select(MenuLayer *ml, MenuIndex *ci, void *ctx) {
  int idx = s_detail_idx;
  if (idx < 0 || idx >= s_count) { return; }
  Timer *t = &s_timers[idx];
  if (ci->row == 0) {                 // Pause / Start -> keep window open
    if (t->state == TS_RUNNING) { tc_pause(t, now_s()); }
    else { tc_start(t, now_s()); }
    persist_all(); rearm_wakeup(); ensure_ticking();
    reload_ui(); menu_layer_reload_data(s_detail_menu);
  } else if (ci->row == 1) {          // Reset -> back to the list
    tc_reset(t, now_s());
    persist_all(); rearm_wakeup(); reload_ui(); select_timer_row(idx);
    window_stack_remove(s_detail_window, true);
  } else if (detail_addable()) {      // +1 / +5 / +10 min -> keep window open
    int32_t secs = (ci->row == 2) ? 60 : (ci->row == 3) ? 300 : 600;
    tc_add(t, secs, now_s());
    persist_all(); rearm_wakeup(); ensure_ticking();
    reload_ui(); menu_layer_reload_data(s_detail_menu);
  }
}

static void detail_window_load(Window *w) {
  Layer *root = window_get_root_layer(w);
  s_detail_menu = menu_layer_create(layer_get_bounds(root));
  menu_layer_set_callbacks(s_detail_menu, NULL, (MenuLayerCallbacks){
    .get_num_rows = dl_num_rows,
    .get_cell_height = dl_cell_height,
    .get_header_height = dl_header_height,
    .draw_header = dl_draw_header,
    .draw_row = dl_draw_row,
    .select_click = dl_select,
  });
  menu_layer_set_normal_colors(s_detail_menu, GColorWhite, GColorBlack);
  menu_layer_set_highlight_colors(s_detail_menu, GColorBlack, GColorWhite);
  menu_layer_set_click_config_onto_window(s_detail_menu, w);
  layer_add_child(root, menu_layer_get_layer(s_detail_menu));
}

static void detail_window_unload(Window *w) {
  menu_layer_destroy(s_detail_menu); s_detail_menu = NULL;
}

static void open_detail_window(int timer_idx) {
  s_detail_idx = timer_idx;
  if (!s_detail_window) {
    s_detail_window = window_create();
    window_set_window_handlers(s_detail_window, (WindowHandlers){
      .load = detail_window_load, .unload = detail_window_unload });
  }
  window_stack_push(s_detail_window, true);
}
```

- [ ] **Step 4: Point `ml_select` at the detail window**

In `ml_select`, change the `else` branch from:

```c
  } else {
    open_action_menu(idx);
  }
```

to:

```c
  } else {
    open_detail_window(idx);
  }
```

- [ ] **Step 5: Build for the watch**

Run: `PEBBLE_QEMU_PATH=~/.local/bin/qemu-pebble-headless pebble build`
Expected: build succeeds on all boards, no references to `action_menu_*` remain. (If the build complains about an unused `state_label`, ignore — it is still used by the LIST window's `ml_draw_row`, unchanged.)

- [ ] **Step 6: Commit**

```bash
git add src/c/main.c
git commit -m "Watch: per-timer detail window (live time, white bg) replaces ActionMenu"
```

---

## Task 2: Emulator screenshot verification (emery + diorite)

**Files:** Temporary edit to `src/c/main.c` (REVERTED at the end — no code change is committed in this task).

The emulator's buttons can't be driven headless, so to capture the detail window, temporarily auto-open it for the seeded "Egg" (running) timer under `SCREENSHOT_FIXTURES`.

- [ ] **Step 1: Add a temporary auto-open in `init`**

In `init`, find the end of the function (after `if (by_wakeup && fired) { trigger_alarm(s_last_fired_idx, fired); }`, before the closing `}`). Add a temporary block:

```c
#ifdef SCREENSHOT_FIXTURES
  open_detail_window(0);   // TEMP: show the detail window for screenshots — REVERT
#endif
```

- [ ] **Step 2: Boot the emery emulator (once)**

Run from the repo root:
```bash
cd /home/dev/pebble-timetracking
scripts/pebble-emu-boot.sh emery
```
Expected: boots within a couple of attempts, then stays up.

- [ ] **Step 3: Build with fixtures, install, screenshot (emery)**

Run (separate calls, do NOT `&&`-chain install+screenshot per CLAUDE.md):
```bash
cd /home/dev/pebble-timetracking/PebbleCountdownTimer
SCREENSHOT_FIXTURES=1 PEBBLE_QEMU_PATH=~/.local/bin/qemu-pebble-headless pebble build
PEBBLE_QEMU_PATH=~/.local/bin/qemu-pebble-headless pebble install   --emulator emery
PEBBLE_QEMU_PATH=~/.local/bin/qemu-pebble-headless pebble screenshot --no-open /tmp/detail-emery.png
```
Expected: a 200×228 PNG showing the detail window — white background, header `Egg   <time>`, rows `Pause / Reset / +1 min / +5 min / +10 min`, first row highlighted (black bg / white text). Surface it to the user with SendUserFile.

- [ ] **Step 4: Screenshot on diorite (144 px fit check)**

Run from repo root then the app dir:
```bash
cd /home/dev/pebble-timetracking
scripts/pebble-emu-boot.sh diorite
cd /home/dev/pebble-timetracking/PebbleCountdownTimer
PEBBLE_QEMU_PATH=~/.local/bin/qemu-pebble-headless pebble install   --emulator diorite
PEBBLE_QEMU_PATH=~/.local/bin/qemu-pebble-headless pebble screenshot --no-open /tmp/detail-diorite.png
```
Expected: a 144×168 PNG; confirm the header + all 5 rows fit (last row may sit at the very bottom). Surface it to the user.

- [ ] **Step 5: Revert the temporary auto-open**

Remove the `#ifdef SCREENSHOT_FIXTURES open_detail_window(0); ... #endif` block added in Step 1.

Run: `git diff --stat` → expected: NO changes to `src/c/main.c` (it matches the Task 1 commit). Confirm with `git status` (clean working tree).

- [ ] **Step 6: (No commit)**

This task produces screenshots, not code. If `git status` shows any leftover change to `main.c`, the revert was incomplete — fix it so the tree is clean.

---

## Task 3: Real-watch verification (manual, user-driven)

**Files:** none.

- [ ] **Step 1: Install on the real Pebble Time 2**

Run: `cd /home/dev/pebble-timetracking/PebbleCountdownTimer && pebble install --cloudpebble build/PebbleCountdownTimer.pbw`
(Note: rebuild WITHOUT `SCREENSHOT_FIXTURES` first if the last build used it — `PEBBLE_QEMU_PATH=~/.local/bin/qemu-pebble-headless pebble build`.)

- [ ] **Step 2: Verify the detail window**

On the watch: open a running timer → header shows the timer name + a time that ticks
down every second; rows read `Pause / Reset / +1 min / +5 min / +10 min`; background
is white with black text, the selected row inverted. Press `+5 min` three times → the
header time grows by 15:00 and the window STAYS open. Press `Pause` → row 0 becomes
`Start` and the time freezes. Press `Reset` → returns to the timer list. Re-open and
press `Back` → returns to the list.

- [ ] **Step 3: Verify auto-return is unchanged for the idle one-tap**

With "Return to watchface after starting a timer" enabled on the phone: from the list,
Select an IDLE timer (one tap) → app exits to the watchface. Confirm the detail
window's `Start` does NOT auto-return (open a paused timer, press `Start`, stays in the
detail window).

- [ ] **Step 4: Record any divergence**

If on-watch behaviour differs from the spec (e.g. row text color on the highlighted
row, or fit on 144 px), note it in
`docs/superpowers/specs/2026-06-15-timer-detail-window-design.md` and commit.

---

## Self-Review Notes

- **Spec coverage:** custom MenuLayer window (Task 1) · header name+live time (Task 1
  Step 3 `dl_draw_header` + tick refresh Step 2) · rows Pause/Reset/+1/+5/+10 with
  Pause↔Start (Step 3 `dl_row_label`/`dl_num_rows`) · white bg/black text + inverted
  highlight (Step 3 `detail_window_load`) · compact 28 px rows (Step 3
  `dl_cell_height`) · Pause/+N keep window open, Reset pops to list (Step 3
  `dl_select`) · auto-return only on idle one-tap (Step 3 `return_to_watchface` kept,
  removed from the deleted `act_toggle`; `ml_select` idle branch unchanged) ·
  ActionMenu removed (Step 3) · screenshots (Task 2) · real watch (Task 3). All spec
  sections map to a task.
- **Type/name consistency:** `s_detail_window`/`s_detail_menu`/`s_detail_idx`,
  `detail_addable`, `dl_num_rows`/`dl_cell_height`/`dl_header_height`/`dl_draw_header`/
  `dl_row_label`/`dl_draw_row`/`dl_select`, `detail_window_load`/`_unload`,
  `open_detail_window` — used consistently. `select_timer_row` and
  `return_to_watchface` are KEPT (not redefined). `ml_select` calls
  `open_detail_window`.
- **One known assumption:** MenuLayer sets the cell text color before `draw_row`, so
  `dl_draw_row` draws in the correct normal/highlight color without setting it. The
  Task 2 screenshots confirm this; if the highlighted row's text is the wrong color,
  set the color explicitly in `dl_draw_row` based on
  `menu_layer_get_selected_index` and re-shoot.
