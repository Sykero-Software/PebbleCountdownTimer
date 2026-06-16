# Quick custom timer — design

Date: 2026-06-16
Status: approved

## Goal

Let the user create and start a one-off "custom" countdown timer directly on the
watch, with no name, and optionally make it permanent. Today every timer comes
from the phone config (`TimerConfig`); the watch can only start/pause/stop/adjust
existing ones. This adds on-watch creation while keeping the phone as the single
source of truth for the saved list.

## UX model

Rather than a separate "Custom" creation mode, reuse the existing per-timer
**detail window**, reached by a new gesture:

- **List, short SELECT** — unchanged: an `IDLE` timer starts immediately (with the
  "Started" confirmation when AutoReturn is on); a running/paused/done timer opens
  its detail window.
- **List, long SELECT** — NEW: opens the target row's detail window in **any**
  state (today the detail window is unreachable for `IDLE` timers, because short
  SELECT starts them directly).

The detail window already carries a live-time header and `+1 min` / `-1 min`
rows. Two actions are added so it doubles as the create/clone surface.

### Detail window rows (always 4 rows)

| State          | row 0           | row 1                  | row 2    | row 3    |
|----------------|-----------------|------------------------|----------|----------|
| idle / done    | `Start`         | `Save as new & start`  | `+1 min` | `-1 min` |
| running        | `Pause`         | `Stop`                 | `+1 min` | `-1 min` |
| paused         | `Start` (resume)| `Stop`                 | `+1 min` | `-1 min` |

- `Save as new & start` is shown only in the startable states (idle/done). Running
  and paused keep today's `Stop` in row 1 (out of scope per design decision).
- If the list is full (`s_count == MAX_TIMERS`, 16) the `Save as new & start` row
  shows a brief "List full" feedback and does not create a new timer (the user can
  still plain-`Start` the adjusted timer).

## Adjusting the duration before starting (idle/done)

`tc_add` is a no-op for `IDLE`/`DONE` (it only adjusts a live or paused countdown).
To let the user tune a duration before starting **without mutating the configured
template**:

- `+1 min` / `-1 min` on an idle/done timer adjust `remaining` by ±60 s, clamped to
  a 60 s floor (`MAX` upper bound = the existing duration cap). `duration` (the
  template) is untouched.
- `tc_start` is changed so a **non-running** timer starts from `remaining`
  (fallback to `duration` when `remaining < 1`). Today it starts `PAUSED` from
  `remaining` and everything else from `duration`; the change extends the
  `remaining` path to `IDLE`/`DONE`. A freshly reset idle timer has
  `remaining == duration`, so a plain Start is unchanged; an adjusted one honors
  the new value. After the run finishes and `tc_reset` runs, `remaining` returns to
  `duration`, so the adjustment is genuinely one-off.

Running/paused `+1 min` / `-1 min` keep the existing `tc_add` live-adjust path.

## "Save as new & start"

1. Read the current (possibly adjusted) `remaining` of the detail timer → that
   becomes the new timer's `duration`.
2. Append a new `Timer` at the **end** of `s_timers` (index `s_count`):
   `name = ""` (unnamed), `duration = <adjusted secs>`, started immediately
   (`tc_start`, so `state = RUNNING`, `end_time = now + duration`), `custom = true`.
   `s_count++`.
3. `persist_all()`, `rearm_wakeup()`, `ensure_ticking()`.
4. Send the new duration to the phone via a new outbox key **`AddTimer`** (seconds,
   `uint32`).
5. Apply the normal start tail: select the new row; with AutoReturn on, show the
   "Started" confirmation then pop to the watchface, else pop back to the list.

Appending at the end (both on the watch and, below, on the phone) is what lets the
later config reconcile align the new entry to this running row by position and keep
its running state, instead of dropping it and re-adding a fresh `IDLE` copy.

## Reconcile preserves watch-created timers (`custom` flag)

Add a `bool custom` field to `Timer`. `tc_reconcile` currently merges config over
runtime **by list position** and drops any runtime row beyond the config count
(`i >= cfgN`). Change it to:

- `i < cfgN`: merge config[i] over cur[i] as today, and clear `custom` on the
  result — the row is now config-backed.
- `i >= cfgN`: if `cur[i].custom`, **keep** it (appended after the config rows);
  otherwise drop it as today.

Effect: a watch-created timer survives every reconcile until the phone's config
grows to include it (at which point it absorbs by position and the flag clears).
This is robust even if the phone was offline when `AddTimer` was sent — the timer
persists locally as `custom` and is absorbed whenever the phone's config catches
up.

### Persistence schema bump

`custom` is watch-local runtime state; it is **not** part of the `TimerConfig`
wire format (name + seconds only). It is persisted with the rest of the `Timer`
via the per-timer persist keys. Adding the field changes `sizeof(Timer)`, so bump
`STORE_SCHEMA` 1 → 2; `store_load` returns 0 for the old schema (one-time reset),
after which the watch re-fetches config from the phone on launch as usual.

## Phone side (`src/ts/index.ts`)

The single `appmessage` listener currently resends the persisted config on any
inbound message (the launch handshake driven by the watch's `Request`). Split on
the payload:

- Payload has a numeric `AddTimer` → read `timer_config` from `localStorage`, parse
  via `stringToTimerList`, append `{ name: '', seconds: AddTimer }` (cap at
  `MAX_TIMERS`), re-serialize via `timerListToString`, and write back. Also update
  the Clay settings store (`clay-settings` `localStorage`) `TimerList` array so the
  config page shows the new timer and a later Save does not clobber it.
  **No echo back to the watch** — the watch already holds the running timer locally
  as `custom`; echoing would trigger a reconcile while it is running. The phone's
  updated config reaches the watch on its next launch handshake.
- Otherwise (`Request`) → existing resend behavior, unchanged.

## Message key

Append `AddTimer` to the **end** of `messageKeys` in `package.json` (IDs are
positional). Run `pebble clean && pebble build` so the `MESSAGE_KEY_AddTimer` C
macro regenerates. PebbleCountdownTimer has no Android companion with hardcoded key
ints, so only the package.json↔generated-C-macro↔PKJS-string-key chain matters;
appending is safe.

## Files touched

- `src/c/main.c` — list `select_long_click` (`ml_select_long`); detail rows/labels
  per state; idle/done `+/-` adjust `remaining`; `Save as new & start` (create +
  `AddTimer` outbox); reuse start confirmation/auto-return tail.
- `src/c/timer_calc.{h,c}` — `Timer.custom`; `tc_start` non-running starts from
  `remaining`; `tc_reconcile` preserves trailing `custom` rows.
- `src/c/timer_store.{h,c}` — `STORE_SCHEMA` 1 → 2 (persist the wider `Timer`).
- `src/ts/index.ts` — `AddTimer` inbound branch (append to `timer_config` +
  `clay-settings` `TimerList`).
- `package.json` — append `AddTimer` to `messageKeys`.
- Tests — `tc_reconcile` (custom-row preservation/absorption), `tc_start`
  (idle/done start from adjusted `remaining`).

## Out of scope

- Naming a custom timer on the watch (unnamed only; rename later on the phone).
- `Save as new & start` for running/paused timers.
- Smooth hold-to-accelerate `+/-` (a `MenuLayer` row cannot repeat-on-hold; would
  need a separate UP/DOWN window — explicitly dropped).
- A dedicated "New timer" list row for from-scratch creation on an empty list
  (creation is always by cloning an existing row via long SELECT).

## Verification

- `npm test` (TS, incl. updated reconcile/start tests).
- Headless emulator (diorite for 144px, emery for the user's Pebble Time 2):
  long-SELECT an idle timer → detail shows Start/Save-as-new/+1/-1; adjust and
  Start runs the adjusted time; Save-as-new appends an unnamed running row.
- Real watch via CloudPebble: confirm `AddTimer` reaches the phone, the timer
  persists across an app close/reopen, and appears on the Clay config page.
