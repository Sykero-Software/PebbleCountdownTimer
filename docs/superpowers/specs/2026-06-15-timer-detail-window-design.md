# Design: timer detail window (replaces the ActionMenu)

Date: 2026-06-15
App: PebbleCountdownTimer ("Sykerö Countdown Timer")

Revises the action-menu portion of `2026-06-15-add-time-actions-and-auto-return-design.md`.
The `tc_add()` core, the `AutoReturn` persist/config plumbing, and the Clay/JS work from
that spec are KEPT; this spec only replaces the on-watch action UI.

## Motivation

User feedback on the shipped ActionMenu (`+1/+5/+10 min`):

1. Pause and Reset should come first, before the `+N` items.
2. The menu should show the timer's current running (remaining) time.
3. Selecting `+N` should NOT auto-return to the watchface — the user may press
   `+1 min` several times in a row.
4. The menu should be compact so the whole thing fits on screen.
5. The dark menu background is disliked — prefer black text on a white background.

The Pebble `ActionMenu` API cannot satisfy 2, 4, or 5: `ActionMenuConfig` has no
title/header field, its `colors.background` only tints the left "crumb" column (not
the item rows, which follow the system theme and render dark), and row layout
(`ActionMenuLevelDisplayMode` Wide/Thin) is not compactable for text actions. So the
ActionMenu is replaced with a custom `MenuLayer`-based window we fully control —
matching the existing in-`main.c` alarm-window pattern.

## What is removed

- `open_action_menu()` and its ActionMenu callbacks `act_toggle`, `act_reset`,
  `act_add_time`, plus `action_menu_did_close` and the `s_action_root` global.
- The `return_to_watchface()` call inside `act_toggle` (auto-return from the menu).

## What stays

- `tc_add()` (timer_calc) — unchanged.
- `tc_start`/`tc_pause`/`tc_reset` (timer_calc) — unchanged.
- `AutoReturn` persist key + Clay toggle + JS plumbing — unchanged.
- `return_to_watchface()` helper — KEPT, but now called ONLY from the idle one-tap
  start path in `ml_select`.

## The detail window (new, in `main.c`)

State:
- `static Window *s_detail_window;`
- `static MenuLayer *s_detail_menu;`
- `static int s_detail_idx = -1;` — config index the window is showing.

### Trigger

`ml_select` keeps its current split:
- `TS_IDLE` timer → `tc_start` directly (no window), then `return_to_watchface()`
  (the ONLY auto-return path).
- otherwise → `open_detail_window(idx)` (replaces `open_action_menu(idx)`).

### Header (live time)

MenuLayer header via `.get_header_height` (returns a compact height, ~26 px) and
`.draw_header`:
- Named timer: name left-aligned + remaining time right-aligned on one line
  (e.g. `Egg            4:32`).
- Unnamed timer: remaining time only, centered.
- Time = `tc_format_remaining(buf, n, tc_remaining_now(t, now_s()))`.
- Font: `FONT_KEY_GOTHIC_18_BOLD`.

### Rows (state-dependent)

| State    | Rows                                    |
|----------|-----------------------------------------|
| RUNNING  | `Pause` `Stop` `+1 min` `-1 min`         |
| PAUSED   | `Start` `Stop` `+1 min` `-1 min`         |
| DONE     | `Start` `Stop`                           |

Row 1 `Stop` resets the timer to idle (`tc_reset`) — named "Stop" for consistency
with the alarm's Stop button. The alarm's `+1 Min` snooze also opens this detail
window for the snoozed timer. Row 0 label is `Pause` when RUNNING else `Start`. The `+1 min` / `-1 min` rows only
exist when RUNNING or PAUSED (`addable`). `get_num_rows` returns `addable ? 4 : 2`.
`-1 min` subtracts a minute (`tc_add(..., -60, ...)`); floored at 0 by `tc_add`
for a paused timer, and for a running timer with under a minute left it reaches the
end and the alarm fires on the next sweep — the user confirmed this behavior.

### Colors + compactness

- `menu_layer_set_normal_colors(s_detail_menu, GColorWhite, GColorBlack)` — white
  background, black text.
- `menu_layer_set_highlight_colors(s_detail_menu, GColorBlack, GColorWhite)` —
  selected row inverted (high contrast, no color dependency so it also looks right
  on b&w diorite).
- Cell height ~28 px (`.get_cell_height`) so header + 4 rows fit comfortably on both
  emery (228 px) and diorite (26 px header + 4×28 = 138).

### Select handler (`detail_select`)

`idx = s_detail_idx`; guard `0 <= idx < s_count`. Dispatch on `ci->row` given the
state's row set:

- **Pause/Start (row 0):** `if RUNNING tc_pause else tc_start`; then
  `persist_all(); rearm_wakeup(); ensure_ticking();` reload main list
  (`reload_ui`) AND reload the detail menu (`menu_layer_reload_data(s_detail_menu)`,
  which also redraws the header and the row-0 label that just toggled). Window stays
  open. NO auto-return.
- **Stop (row 1):** `tc_reset(...)`; `persist_all(); rearm_wakeup(); reload_ui();`
  then `window_stack_remove(s_detail_window, true)` to pop back to the list (the
  timer is now IDLE; the detail window has no IDLE row set). The list cursor follows
  via `select_timer_row(idx)` before popping.
- **`+1 min` / `-1 min` (rows 2-3, only when addable):** `tc_add(..., 60, ...)` /
  `tc_add(..., -60, ...)`; `persist_all(); rearm_wakeup(); ensure_ticking();`
  `reload_ui()` + `menu_layer_reload_data(s_detail_menu)` (header time jumps). Window
  stays open so repeated presses work.

Row→seconds is a small switch (row 2 → +60, row 3 → -60); no action_data plumbing
(that was an ActionMenu concept).

### Live ticking while the window is open

`tick_cb` already runs every second while any timer is RUNNING and calls
`reload_ui()`. Extend it: after `reload_ui()`, if `s_detail_window` is on top and
`s_detail_menu` exists, call `menu_layer_reload_data(s_detail_menu)` to retick the
header. A paused timer doesn't tick (no running timer ⇒ no timer fired), which is
correct — paused remaining is static. `ensure_ticking` is unchanged (keys off any
RUNNING timer regardless of which window is showing).

### Lifecycle

- `open_detail_window(idx)`: set `s_detail_idx = idx`; lazily `window_create` +
  set handlers + `window_set_click_config` via `menu_layer_set_click_config_onto_window`;
  `window_stack_push`. On `.load` create the MenuLayer with the callbacks + colors;
  on `.unload` destroy it and null both globals.
- Back button pops the window (standard MenuLayer click config) → returns to list.
- If the showing timer expires while the window is open, the existing alarm flow
  (`trigger_alarm`) pushes the alarm window on top — unchanged; on dismiss the
  detail window is underneath. (Acceptable; no special handling.)

## Components / boundaries

All in `src/c/main.c`, mirroring the existing alarm-window section (which is also an
in-`main.c` Window + handlers with no separate module). New section "timer detail
window" sits near the menu callbacks. `main.c` grows to ~520 lines — consistent with
the file's established style (it already holds the list window + alarm window).

## Error handling / edge cases

- `s_detail_idx` bounds-checked in every handler (timers can be removed by a config
  reconcile while the window is open; a stale index is ignored).
- Reconcile (`inbox_received`) does not close the detail window; the next
  `menu_layer_reload_data` (tick or action) re-reads `s_timers[s_detail_idx]` with
  the bound check, so a shrunk list is handled gracefully.

## Testing / verification

- The UI layer gets NO host unit test, matching the alarm-window precedent (button
  wiring + MenuLayer rendering have no pure-logic surface). `tc_add` and the timer
  transitions are already host-tested.
- `npm test` + the `gcc` core test must stay green (unchanged).
- `pebble build` succeeds on all boards.
- Screenshots: the emulator's buttons can't be driven headless, so to capture the
  detail window, temporarily open it for a seeded timer under `SCREENSHOT_FIXTURES`
  in `init` (build/install/shoot, then REVERT the temporary hook). Capture on emery
  (200 px, the user's real watch) and diorite (144 px) to confirm fit + colors.
  Surface screenshots to the user.
- Real Pebble Time 2: open a running timer → header shows live ticking time, rows
  `Pause/Reset/+1 min/-1 min`, white bg/black text; press `+1 min` a few times → time
  grows and the window stays open; press `-1 min` → time drops by a minute; Pause →
  row 0 becomes `Start`, time freezes; Reset → back to the list; Back → back to the
  list; starting an idle timer with AutoReturn on still exits to the watchface.

## Out of scope

- Changing the main list window, the alarm window, or any non-UI behavior.
- `+N` on Done/Idle timers (unchanged: Done has no +N; Idle one-taps to start).
- Configurable increments or colors (fixed).
