# Detail-menu actions: Stop default, no-duplicate save, Delete, exit reason — design

Date: 2026-06-16
Status: approved

## Goal

Four focused improvements to the per-timer **detail window** and app exit
behaviour, all driven by real on-watch use:

1. **No duplicate on unchanged "Start & Save".** Pressing the save action without
   adjusting the duration today appends an identical copy of the timer. It must
   instead just start the existing timer in place.
2. **Delete a timer from the detail window**, with a confirmation screen, removing
   it both on the watch and from the phone config.
3. **Exit to the watchface, not the launcher.** Every action that closes the app
   must set the exit reason so PebbleOS returns to the watchface.
4. **Stop is the default (topmost) action** for running/paused timers, not Pause.

These build on the existing detail window (`docs/.../2026-06-15-timer-detail-window-design.md`)
and quick-custom-timer work (`docs/.../2026-06-16-quick-custom-timer-design.md`).

## Detail-window row model (rebuilt per state)

Today the four detail rows are hardcoded by index in `dl_num_rows` /
`dl_row_label` / `dl_select`. With Stop moving to the top, a dynamic Delete row,
and the save row appearing only conditionally, the row count is no longer fixed
(4 or 5). Replace the index switches with a **per-state action list**.

### Action enum

```
typedef enum {
  DACT_STOP,        // running/paused -> reset to IDLE
  DACT_PAUSE,       // running -> pause
  DACT_START,       // idle/done/paused -> start/resume in place
  DACT_SAVE_START,  // idle/done, time changed -> create new custom timer + start
  DACT_PLUS,        // +1 min
  DACT_MINUS,       // -1 min
  DACT_DELETE,      // delete this timer (with confirm)
} DetailAction;
```

### Ordering per state

| State                       | rows (top → bottom)                              |
|-----------------------------|--------------------------------------------------|
| running                     | Stop, Pause, +1 min, -1 min, Delete              |
| paused                      | Stop, Start (resume), +1 min, -1 min, Delete     |
| idle / done — time changed  | Start, Start & Save, +1 min, -1 min, Delete      |
| idle / done — unchanged     | Start, +1 min, -1 min, Delete                    |

- **Stop is row 0** for running/paused (req 4) — the cursor lands on it, so a quick
  SELECT stops/resets. Stop is recoverable (`tc_reset` keeps `duration`).
- **Delete is always the last row** (req 2) — destructive, least frequent.
- **"Start & Save" appears only when the time was changed** (req 1, see below).
- Labels: `Stop`, `Pause`, `Start`, `Start & Save`, `+1 min`, `-1 min`, `Delete`.

### Pure helper for the action list (testable)

Add to `timer_calc.{h,c}` so it is unit-testable without the SDK:

```
// Fill `out` with the ordered detail-window actions for a timer in `st` whose
// duration was/was not changed before starting (`changed`). Returns the count.
int tc_detail_actions(TimerState st, bool changed, DetailAction *out);

// True when an idle/done timer's start time differs from its template duration,
// i.e. the user adjusted it with +/- (so "Start & Save" is meaningful). Always
// false for running/paused (which use Stop, not the save action).
bool tc_detail_changed(const Timer *t);
```

`tc_detail_changed` returns, for `IDLE`/`DONE`:
`intended != t->duration`, where `intended = (t->remaining >= 1 ? t->remaining : t->duration)`.
A freshly reset idle timer has `remaining == duration` → `false`. After ±1 min it
differs → `true`. Returns `false` for `RUNNING`/`PAUSED`.

`DetailAction` lives in `timer_calc.h` so the helper and `main.c` share it.

`dl_num_rows`, `dl_row_label`, and `dl_select` all consult one cached
`DetailAction s_detail_acts[7]` + `s_detail_act_count`, rebuilt (via
`tc_detail_actions`) whenever the detail menu reloads.

## Req 1 — no duplicate on unchanged "Start & Save"

The duplicate is eliminated structurally by the row model: **`DACT_SAVE_START` is
only present when `tc_detail_changed(t)` is true.** When the user hasn't adjusted
the time, the save row simply isn't shown, so there is no way to create an
identical copy — `DACT_START` (row 0) starts the existing timer in place.

`DACT_SAVE_START` keeps today's behaviour (`save_as_new_and_start` with the
adjusted seconds), since by construction the new timer differs from the template.
`save_as_new_and_start` is otherwise unchanged.

## Req 2 — Delete with confirmation + phone sync

### Watch

- `DACT_DELETE` opens a small **confirm window** (own `Window`, red-free, neutral):
  - Title line: `Delete?`
  - Sub line: the timer name, or its formatted time when unnamed.
  - Button hints: SELECT = delete, Back = cancel (Back also cancels by default).
  - Click config: `BUTTON_ID_SELECT` → confirm-delete; Back is the implicit pop.
- On confirm:
  1. `send_delete_timer(s_detail_idx)` — tell the phone (best-effort, see below).
  2. Remove the timer locally: shift `s_timers[idx+1..]` down one, `s_count--`.
     (New helper `remove_timer_at(int idx)`.)
  3. `persist_all(); rearm_wakeup();` (a deleted running timer's wakeup is dropped).
  4. `reload_ui();`
  5. Return to the **list**: pop the confirm window and the detail window
     (`window_stack_remove` both, or `window_stack_pop` twice). NOT the watchface —
     delete is a management action, the user keeps managing the list. (AutoReturn
     does not apply to delete.)
- The confirm window is created lazily and reused (like `s_confirm_window`),
  destroyed in `deinit` if allocated.

### Phone (PKJS)

- New message key **`DeleteTimer`** (value = the watch list index to remove),
  **appended last** in `package.json` `messageKeys` (positional/append-only rule;
  run `pebble clean && pebble build` so the `MESSAGE_KEY_*` C macros regenerate).
- New module `src/ts/delete_timer.ts`:
  ```
  export function deleteTimer(get, set, index): string | null
  ```
  mirrors `add_timer.ts`: load `timer_config` → splice `index` (bounds-checked) →
  write back `timer_config` and `clay-settings.TimerList`. Returns the new config
  string, or `null` when the index is out of range.
- `index.ts` `appmessage` handler: when `typeof p.DeleteTimer === 'number'`, call
  `deleteTimer` and log the result; no echo (the watch already removed it locally).
  This branch sits alongside the existing `AddTimer` branch, before the resend
  fallback.

### Index alignment

Watch `s_timers[i]` and the phone's `timer_config` list are aligned by position
(`tc_reconcile` is positional; custom rows append in the same order on both
sides). So the watch list index is a valid index into the phone list. Splicing the
same index on both sides preserves alignment for the remaining timers.

### Reliability (best-effort, same as AddTimer)

If the `DeleteTimer` send fails (phone offline / app not paired), the watch still
removes the timer locally, but the phone keeps it, so the **next config reconcile
re-adds it**. This matches the existing `AddTimer` best-effort contract and is
acceptable for v1. No retry/queue is added. (Documented, not silently dropped.)

## Req 3 — exit to the watchface

`exit_reason_set` is currently called **nowhere**, so every `window_stack_pop_all`
returns to wherever the app was launched from (the launcher), not the watchface.

Add a helper and route all app-closing exits through it:

```
static void close_to_watchface(void) {
  exit_reason_set(APP_EXIT_ACTION_PERFORMED_SUCCESSFULLY);
  window_stack_pop_all(true);
}
```

Replace the three current `window_stack_pop_all(true)` call sites:

- `alarm_stop` (Stop from the full-screen alarm).
- `dl_select` Stop branch when `s_auto_return` is set.
- `confirm_timer_cb` (the "Started" confirmation auto-close).

The `window_stack_remove(...)` sites (back to the list — `alarm_add_minute`,
`dl_select` Stop without auto-return, `save_as_new_and_start` without auto-return)
are **not** app exits and are left unchanged. Delete's return-to-list (req 2) also
uses `window_stack_remove`, not the exit helper.

## Testing

- **C unit tests** (`tests/test_timer_calc.c`):
  - `tc_detail_changed`: idle unchanged → false; idle after +60 s → true; done with
    `remaining==0` → false; running/paused → false.
  - `tc_detail_actions`: each of the four state/changed combinations yields the
    exact ordered action list above (incl. Stop first for running/paused, Delete
    last, Save row present iff changed).
- **PKJS tests** (`tests/delete_timer.test.js`, mirroring `add_timer.test.js`):
  delete middle/first/last index updates `timer_config` and `clay-settings`;
  out-of-range index returns `null` and leaves storage untouched.
- **No `check-pebble-message-keys.py` change** — that script covers TimeStyle and
  PebbleTrackWorkTime, not PebbleCountdownTimer. The append-only rule still applies
  to `DeleteTimer`.
- **Manual (emulator/diorite)**: detail menu shows Stop on top for running; Start &
  Save absent until +/- pressed; Delete → confirm → list updates; Stop from alarm
  and the "Started" flash both land on the watchface.

## Out of scope

- Delete retry/queue when the phone is offline (best-effort only).
- Deleting multiple timers at once / list-level delete gesture.
- Any change to the list window's short/long SELECT behaviour.
