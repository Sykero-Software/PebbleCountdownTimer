# Design: +N min action-menu items & auto-return-to-watchface

Date: 2026-06-15
App: PebbleCountdownTimer ("Sykerö Countdown Timer")

Two independent features for the countdown timer watchapp:

1. **Add-time actions** — `+1 min` / `+5 min` / `+10 min` in a timer's action menu,
   available while the timer is **Running or Paused**.
2. **Auto-return to watchface** — a config-page toggle that exits the app back to
   the watchface after a timer is started/resumed.

Both follow existing patterns in the codebase (action-menu callbacks; Clay setting
→ AppMessage → persist), so no new architecture is introduced.

---

## Feature 1 — `+1 / +5 / +10 min` action-menu items

### Behaviour

- The three items are shown in the per-timer action menu **only when the timer is
  `TS_RUNNING` or `TS_PAUSED`**. They are hidden for `TS_DONE` (and `TS_IDLE` never
  opens the menu — Select starts an idle timer directly).
- Semantics is **additive** relative to the time currently left:
  - Running: extend the live countdown (`end_time += N*60`), e.g. 3:00 left + 5 → 8:00.
  - Paused: grow the frozen remainder (`remaining += N*60`).
- Each action stamps `last_used = now` (it is a user action; matches Start/Pause/Reset),
  so the timer re-sorts to the top in MRU mode and the cursor follows it.

### Menu layout (flat list)

`open_action_menu` becomes state-aware:

| State    | Items                                            | Count |
|----------|--------------------------------------------------|-------|
| Running  | `Pause` `+1 min` `+5 min` `+10 min` `Reset`      | 5     |
| Paused   | `Start` `+1 min` `+5 min` `+10 min` `Reset`      | 5     |
| Done     | `Start` `Reset`                                  | 2     |

`action_menu_level_create(count)` is sized from the state.

### Core logic — `timer_calc.c` / `timer_calc.h`

New pure function (host-testable, no Pebble syscalls):

```c
// Add `secs` to the time left, regardless of running/paused. RUNNING: end_time +=
// secs. PAUSED: remaining += secs (clamped >=0). Other states: no-op. Stamps last_used.
void tc_add(Timer *t, int32_t secs, int64_t now);
```

This is **separate from** the existing `tc_extend` (which *sets* `end_time = now+secs`
and forces RUNNING — used by the alarm's "+1 Min" snooze on a finished timer).
`tc_extend` is unchanged.

### Wiring — `main.c`

- One shared callback `act_add_time` reads the seconds from the menu item's
  `action_data` (set via the 4th arg of `action_menu_level_add_action`, read back
  with `action_menu_item_get_action_data`) and the timer index from
  `action_menu_get_context`, then:
  `tc_add` → `persist_all` → `rearm_wakeup` → `ensure_ticking` → `reload_ui` →
  `select_timer_row(idx)` (identical tail to `act_toggle`).
- `+1/+5/+10 min` pass `(void*)(intptr_t)60 / 300 / 600` as `action_data`.

### Tests

Add `tc_add` cases to `tests/test_timer_calc.c`:
- Running: `end_time += secs`, `last_used` stamped, `tc_remaining_now` grows.
- Paused: `remaining += secs`, stays `TS_PAUSED`.
- Idle/Done: no change.

---

## Feature 2 — auto-return to watchface after start

### Behaviour

A config toggle "Return to watchface after starting a timer" (default **off**).
When on, the app exits to the watchface (`window_stack_pop_all`) after a timer
transitions to **RUNNING**, on **any start/resume**:

- `ml_select` idle-tap path (Select on an idle timer → `tc_start`).
- `act_toggle` when the result is `TS_RUNNING` (the menu "Start" on a paused/done timer).

Not triggered by Pause, Reset, or the new `+N` actions.

The timer keeps running while the app is closed via the existing wakeup mechanism
(`rearm_wakeup` in `deinit`), so the full-screen alarm still fires on expiry.

### Config page — `config_clay.ts`

Add to the **Display** section:

```ts
{ type: 'toggle', messageKey: 'AutoReturn',
  label: 'Return to watchface after starting a timer', defaultValue: false }
```

### Message key — `package.json`

**Append** `"AutoReturn"` (append-only; this app's keys are name-matched between the
C `MESSAGE_KEY_*` macros and the JS dict, so order does not drift the way the
Android-side positional ints do — but appending is still the rule):

```json
"messageKeys": ["TimerConfig", "SortOrder", "Request", "AutoReturn"]
```

> GOTCHA: after editing `messageKeys`, run `pebble clean && pebble build` — a plain
> build keeps the cached `MESSAGE_KEY_*` header and the C side fails with
> `'MESSAGE_KEY_AutoReturn' undeclared`.

### Phone JS

- `index.ts` `webviewclosed`: `dict.AutoReturn = s.AutoReturn ? 1 : 0;` and persist
  `window.localStorage.setItem('auto_return', String(dict.AutoReturn))`.
  (Clay `toggle` returns a boolean from `getSettings`; the watch reads an int, so
  send `0/1`.)
- `config_sync.ts` `resendDict`: include
  `AutoReturn: parseInt(get('auto_return') || '0', 10) || 0` in the launch-handshake
  reply (so the setting survives an app relaunch like `SortOrder` does).

### C side — persist + apply

- `timer_store.h`: `#define PERSIST_KEY_AUTORETURN 5`; declare
  `bool store_load_autoreturn(void)` / `void store_save_autoreturn(bool)`.
- `timer_store.c`: read/write via `persist_read_bool` / `persist_write_bool`
  (default `false` when unset).
- `main.c`:
  - `static bool s_auto_return;`
  - `init`: `s_auto_return = store_load_autoreturn();`
  - `inbox_received`: if `dict_find(iter, MESSAGE_KEY_AutoReturn)`, set
    `s_auto_return = tuple->value->int32 != 0;` then `store_save_autoreturn`.
  - helper `static void return_to_watchface(void) { if (s_auto_return) window_stack_pop_all(true); }`
    called at the end of the idle-`tc_start` path in `ml_select` and in `act_toggle`
    when the timer is now `TS_RUNNING`.

---

## Verification

- `npm test` (TS/JS) green.
- `gcc -I src/c tests/test_timer_calc.c src/c/timer_calc.c -o /tmp/tc_test && /tmp/tc_test`
  green (includes new `tc_add` cases).
- `pebble clean && pebble build`; screenshots on **emery** (200px, the user's real
  watch) and **diorite** (144px) of the action menu showing the `+N` items for a
  running and a paused timer.
- Real Core Devices watch: enable the toggle, start a timer → app exits to
  watchface; confirm the timer still alarms at zero. Use a `+N` action and confirm
  the remaining time grows.

## Out of scope

- `+N` on Done/Idle timers (Done uses the alarm's snooze; Idle just starts).
- Configurable increment values — fixed at 1/5/10 min per request.
- Auto-return on pause/reset.
