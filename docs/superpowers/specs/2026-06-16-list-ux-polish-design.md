# PebbleCountdownTimer — list UX polish (design)

Date: 2026-06-16
Status: approved

Three small UX improvements to the watchapp list/start experience:

1. A brief "started" confirmation screen when starting a timer closes the app.
2. State-colored row backgrounds in the timer list.
3. An optional "running timers first" list ordering (config toggle, default on).

## 1. Start confirmation screen

**Goal:** when the user one-taps an idle timer to start it and the app then
closes to the watchface, flash a short confirmation so the user understands what
happened.

**When it shows:** ONLY on the idle one-tap start path (`ml_select` on a
`TS_IDLE` timer) AND only when `AutoReturn` is on (the app is about to close). When
`AutoReturn` is off, behavior is unchanged — the list stays visible and already
shows the timer as `Running` (and floats it up), so no confirmation is needed. The
detail-window Start/Resume path (`dl_select` row 0) keeps the window open and is
unaffected.

**Flow:**
- `tc_start(&s_timers[idx], now)` → `persist_all()` / `rearm_wakeup()` /
  `ensure_ticking()` / `reload_ui()` / `select_timer_row(idx)` (unchanged), then
- instead of `return_to_watchface()` calling `window_stack_pop_all` directly:
  push a new confirmation window and register an `app_timer` (~1100 ms) whose
  callback calls `window_stack_pop_all(true)`.

**Content (full-screen confirmation window):**
- Large `✓` glyph (or a bold check drawn with a system font), centered.
- Timer name (or the formatted duration "M:SS" if the timer is unnamed).
- The duration "M:SS" (the value the countdown started from).
- The word "Started".
- Background: dim green on color displays (`GColorMintGreen`, matching the
  running row tint below); white on b&w. Text black.

**Cancellation / safety:**
- The scheduled `app_timer` handle is stored in a static. The confirmation
  window's `unload` handler cancels it (`app_timer_cancel`) so that if the user
  presses Back during the ~1.1 s, the app simply returns to the list and the
  pending `pop_all` does not fire afterwards. (A late `window_stack_pop_all` would
  be harmless but we cancel for clarity.)
- The confirmation window is its own window pushed above the list; the timer
  callback pops the whole stack.

## 2. State-colored row backgrounds (list view)

**Goal:** distinguish running / paused / done timers at a glance in the list.

`ml_draw_row` (main.c) currently calls `menu_cell_basic_draw`. Change it to:

1. Determine whether this row is the selected/highlighted row
   (`menu_index_compare(ci, menu_layer_get_selected_index(s_menu)) == 0`, or the
   equivalent). If selected, draw exactly as today (let MenuLayer's standard
   highlight stand — black background, white text) so the cursor stays obvious.
   Do NOT tint the selected row.
2. For non-selected rows, fill the cell rect with the state tint, set text color
   black, then draw the title/subtitle (reuse the same name/time/state strings as
   today).

**Tints (color displays only):**
- `TS_RUNNING` → `GColorMintGreen` (dim green)
- `TS_PAUSED`  → `GColorPastelYellow` (dim yellow)
- `TS_DONE`    → `GColorMelon` (dim red) — signals the alarm has fired
- `TS_IDLE`    → white (unchanged)

Wrap the tint in `PBL_IF_COLOR_ELSE(<tint>, GColorWhite)` so b&w boards
(diorite/aplite) fall back to plain white (no useful pale tint there). The target
hardware is the Pebble Time 2 (emery, color); b&w stays graceful/unchanged.

**Verification:** seed a couple of timers in different states and screenshot on
the emery emulator (per CLAUDE.md headless recipe). Surface the screenshot to the
user.

## 3. "Running timers first" ordering (config toggle, default on)

**Goal:** intuitively float running timers to the top of the list, independent of
the chosen sort mode. Optional via the config page, default ON.

**Config (Clay, `src/ts/config_clay.ts`):** add a `toggle` with
`messageKey: 'RunningFirst'`, `defaultValue: true`, in the Display section (near
the existing `SortOrder` radiogroup / `AutoReturn` toggle). Label e.g. "Show
running timers at the top".

**Message key:** APPEND `"RunningFirst"` to the END of `messageKeys` in
`package.json` (positional-ID drift gotcha — never insert/reorder). After editing:
run `scripts/check-pebble-message-keys.py` and `pebble clean && pebble build` (the
`MESSAGE_KEY_*` C macros are cached otherwise).

**Phone side (`src/ts/index.ts`):** in the settings→dict mapping add
`dict.RunningFirst = s.RunningFirst ? 1 : 0;` and mirror to localStorage
(`window.localStorage.setItem('running_first', String(dict.RunningFirst))`),
matching the existing `SortOrder` / `AutoReturn` handling.

**Watch side:**
- `timer_store.{h,c}`: `#define PERSIST_KEY_RUNNINGFIRST 6`; add
  `bool store_load_runningfirst(void)` (default **true** when unset) and
  `void store_save_runningfirst(bool on)`.
- `main.c`: a `static bool s_running_first` loaded at init; `inbox_received`
  reads `MESSAGE_KEY_RunningFirst` (when present) and persists it; pass it into
  `rebuild_order()`.
- `timer_calc.{h,c}`: extend `tc_display_order` with a `bool running_first`
  parameter. When true, running timers sort above all non-running timers while
  preserving intra-group order. Implementation: in the comparison key, add a large
  bias (e.g. `1LL << 40`, which dominates `last_used` ≈ epoch secs < 2^31 and any
  `remaining` < 2^31) to running timers' `order_key`. Within each group the
  existing MRU/shortest/longest order is unchanged. Non-running states
  (paused/done/idle) all fall into the lower group.

**Tests:** update/extend `timer_calc` tests to cover: running-first on (running
group floats above non-running, intra-group order preserved for each SortMode) and
running-first off (current behavior unchanged).

## Out of scope / non-goals

- No changes to the alarm window, detail window actions, or wakeup logic.
- No b&w-specific accent indicator (color tint only; b&w unchanged).
- The confirmation screen is intentionally NOT shown on the detail-window
  Start/Resume path or when AutoReturn is off.

## Affected files

- `PebbleCountdownTimer/src/c/main.c` — confirmation window + timer; `ml_draw_row`
  tints; `s_running_first` load/inbox; `rebuild_order` arg.
- `PebbleCountdownTimer/src/c/timer_calc.{h,c}` — `tc_display_order` running-first.
- `PebbleCountdownTimer/src/c/timer_store.{h,c}` — `RunningFirst` persistence.
- `PebbleCountdownTimer/src/ts/config_clay.ts` — `RunningFirst` toggle.
- `PebbleCountdownTimer/src/ts/index.ts` — send/persist `RunningFirst`.
- `PebbleCountdownTimer/package.json` — append `RunningFirst` to `messageKeys`.
- `PebbleCountdownTimer/tests/*` — `tc_display_order` ordering tests.
- Superrepo gitlink bump after committing in the submodule.
