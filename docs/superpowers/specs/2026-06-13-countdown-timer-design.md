# Countdown Timer — design

**Status:** approved design (2026-06-13)
**Repo:** `PebbleCountdownTimer/` (private submodule of the pebble-timetracking superrepo)

## Goal

A simple standalone Pebble watchapp, **"Countdown timer"**. The user configures
a list of named countdown timers (name + duration) entirely from the phone (Clay
config page). On the watch the user sees that list and can only **start / pause /
reset** each timer. Multiple timers may run simultaneously. The app behaves as a
background app: a running timer alerts (vibrates) when it reaches zero even if the
app is not in the foreground.

## Non-goals

- No creating/editing of timers on the watch — that is configuration only, done on
  the phone.
- No PebbleKit2 / no dependency on the trackworktime (or any) Android app. The app
  is fully standalone; "Android-side configuration" means the **Clay config page**,
  which opens in the phone's Core Devices app.
- No `companionApp` declaration (only needed when routing AppMessages to a native
  Android app via PebbleKit2).

## App identity & build

- `package.json` `pebble`:
  - `displayName: "Countdown timer"`, fresh `uuid` (generate one), `sdkVersion: "3"`.
  - `watchapp.watchface: false`.
  - `targetPlatforms`: `aplite, basalt, chalk, diorite, emery, flint, gabbro` (same
    set as PebbleTrackWorkTime).
  - `enableMultiJS: true`, dependency `pebble-clay@^1.0.4`.
  - `messageKeys`: a single `TimerConfig` string key (see Data flow). Append-only if
    more are ever added.
- `wscript`: copy PebbleTrackWorkTime's `wscript` but:
  - **keep** `patch_clay_for_new_platforms` (flint/gabbro need `.a` stubs — Clay
    1.0.4 ships its C lib only for aplite/basalt/chalk/diorite/emery).
  - **drop** `inject_companion_app` (no companionApp here).
- TypeScript Clay setup mirrored from TimeStyle: `src/ts/*.ts` → `tsc` → generated
  `src/pkjs/*.js` (gitignored), `tsconfig.json` (`typescript@^6`, `target: ES5`,
  `ignoreDeprecations: "6.0"`), a `wscript` `compile_typescript` hook running `tsc`
  at the top of `build()` (`noEmitOnError`), `npm test`/`npm run typecheck`.

## Configuration (Clay, phone-side)

A single **reorderable list** custom component, modeled on TimeStyle's
`config_widget_list` (`src/ts/config_widget_list.ts`):

- Each row: **name** text input + **duration** as three small number fields
  (HH / MM / SS) + **↑ / ↓ / ✕** controls.
- An **"Add timer"** button; **max 8** rows (hide Add at 8).
- Value = ordered array of `{ name: string, seconds: number }`.
- Clay-`toSource()` rules (same as TimeStyle's custom component & custom fn): the
  component and any custom fn are serialized via `toSource()` and re-eval'd isolated
  in the config webview — **no TS downlevel helpers** (`__spreadArray`/`__assign`/
  captured `_this`), **no spread/destructuring**, reference only `this`, locals, and
  instance props stashed in `initialize`. Inline everything; native array methods
  only. After building, grep generated JS for `__`/`_this` — must be none in the
  serialized pieces.
- Row buttons must set `min-width: 0` (Clay base theme forces `button{min-width:12rem}`),
  `<select>`/inputs themed dark (`background:#767676;color:#fff;color-scheme:dark`)
  to match Clay's dark page.

### Clay defaults & migration

- Default config (fresh install): a small starter list is acceptable, or empty. C
  `Settings`-equivalent defaults and Clay `defaultValue` must agree (defaults are
  what the emulator shows since the config page can't open headless).
- No legacy-key migration needed (new app, single key from day one).

## Data flow: config → watch

- On `webviewclosed`, the handler serializes the list to **one CString** under
  `TimerConfig`:
  - **records** separated by `\x1e` (RS), **fields** within a record by `\x1f`
    (US): `name \x1f seconds \x1e name \x1f seconds ...`. Names may not contain
    `\x1e`/`\x1f` (strip on the JS side). The watch parser must match exactly.
  - names are truncated to `NAME_LEN` on the watch; seconds is a decimal integer.
- The watch parses the string into its in-memory `Timer[]` config on inbox receive,
  persists it, and reconciles runtime state (below).
- Only Clay JS references message keys (by name), so the positional
  message-key-ID drift documented in the superrepo CLAUDE.md does **not** apply
  (that only bites when a native Android app hardcodes the int IDs).

## Watch app UI

- `Window` + `MenuLayer`, one section, one row per configured timer.
- **Row order = most-recently-used first.** Rows are sorted by each timer's
  `last_used` timestamp (descending); never-used timers fall to the bottom in
  config order (ties broken by config position). The list is a *view* over the
  config — the underlying timer identity is still its config position; only the
  displayed order changes. The order is recomputed when the list rebuilds (on
  open, after an action, on config/state change) — it does not reshuffle live
  under the user's cursor mid-interaction beyond a normal `menu_layer_reload_data`.
- Row content: timer **name** (top line) + **remaining time** formatted `MM:SS`
  (or `H:MM:SS` when ≥ 1 h) + a small **state** indicator: Running / Paused / Done /
  Idle.
- If config is empty: a single info row ("Configure timers on your phone"), never a
  blank screen.
- `Select` click opens an **`ActionMenu`** scoped to the selected timer:
  - **Start** / **Pause** (one item, label depends on current state),
  - **Reset** (back to full duration, state → idle).
- `Up`/`Down` navigate the menu; `Back` exits the app.
- While in foreground, a single 1 s `app_timer` ticks the UI and reloads the visible
  rows; running timers recompute remaining = `end_time − now`.

## Timer state model (persisted)

Per timer (index = list position), held in memory and mirrored to persist storage:

```
state    ∈ { IDLE, RUNNING, PAUSED, DONE }
end_time : time_t   // absolute UTC; valid when RUNNING
remaining: int32_t  // seconds left; valid when PAUSED
duration : int32_t  // configured full length (from config)
last_used: time_t   // last time the user acted on this timer; drives list order
name     : char[NAME_LEN+1]
```

Transitions (each sets `last_used = now`, so acting on a timer floats it to the top
of the watch list):
- **Start** (from IDLE/PAUSED): `end_time = now + remaining` (or `+ duration` from
  IDLE), state → RUNNING. Re-arm wakeup.
- **Pause** (from RUNNING): `remaining = end_time − now` (clamped ≥ 0), state →
  PAUSED. Re-arm wakeup.
- **Reset** (any state): state → IDLE, `remaining = duration`. Re-arm wakeup.
- **Expiry** (`end_time ≤ now` while RUNNING): state → DONE, remaining 0, vibrate.
  Stays DONE at 00:00 until the user resets.

Persistence:
- On every state change and on app exit (`window_unload`/`deinit`), write the array
  to persist storage (a packed struct or per-field keys; small, ≤ 8 entries).
- On launch, load persisted state, then **reconcile** against the (also persisted)
  config and against `now` (see below).

### Config reconciliation

When a new `TimerConfig` arrives or on launch:
- Match by list position. For an unchanged row, keep its runtime state.
- If a row's duration changed: keep state but clamp `remaining`/`end_time`
  sensibly (e.g. IDLE adopts new duration; RUNNING keeps its `end_time`).
- Rows removed (list shorter) drop their state; new rows start IDLE at full
  duration.
- After reconciliation, **mark any RUNNING timer with `end_time ≤ now` as DONE**
  (covers expiries that happened while the app/wakeup was idle) and re-arm wakeup.

## Background alerting — single Wakeup

Constraints (from SDK `pebble.h`): an app may schedule **≤ 8** wakeup events, and
**no two within 1 minute** of each other. Rather than one wakeup per timer (which
collides for round/equal durations), keep **exactly one** scheduled:

- The single wakeup time = **min `end_time` over all RUNNING timers**, scheduled with
  `wakeup_schedule(t, cookie, notify_if_missed=true)`.
- Maintain it: on any Start/Pause/Reset/expiry/config change, `wakeup_cancel`
  the old one (track its `WakeupId` in persist) and re-schedule for the new soonest
  `end_time` (or cancel entirely if none running).
- **On wakeup launch** (`launch_reason() == APP_LAUNCH_WAKEUP`, confirmed via
  `wakeup_get_launch_event`): mark **every** RUNNING timer with `end_time ≤ now` as
  DONE, vibrate (long / double pulse), then re-arm the single wakeup for the next
  future `end_time`. Handles simultaneous and near-simultaneous (<1 min) expiries
  without hitting either limit.
- **Foreground expiry**: the 1 s `app_timer` detects `end_time ≤ now`, vibrates
  immediately, marks DONE; the wakeup is purely the closed-app safety net.
- Edge: if scheduling ever returns an error (e.g. the chosen minute conflicts with
  another app), nudge the target time forward to the next free minute and log it.

## Testable units

Factor pure logic so it can be unit-tested off-device:

**Clay / TS (Jest, like TimeStyle `tests/`):**
- `configToString` / `stringToConfig` round-trip (the AppMessage serialization).
- `listToConfig` / `configToList` for the custom component (trailing-empty trim,
  max-8 cap, name/seconds coercion).
- duration ↔ HH/MM/SS field conversion.

**Watch C (pure helpers, no UI deps):**
- `format_remaining(seconds) → "MM:SS" | "H:MM:SS"`.
- `parse_config_string(buf) → Timer[]`.
- `soonest_end_time(Timer[]) → time_t | NONE` (next-wakeup selection).
- `display_order(Timer[]) → int[]` (indices sorted by `last_used` desc, never-used
  last in config order — drives the MRU list ordering).
- `reconcile(config, persisted_state, now) → Timer[]` (expiry + config diff).

Device verification: emery emulator screenshots of the list (Running/Paused/Done/
Idle rows) per the superrepo CLAUDE.md headless recipe; seed a couple of timers in
the empty-config branch to screenshot without a phone, then revert the seed.

## Open items / risks

- Persist storage layout (packed struct vs per-key) — decide in the plan; keep it
  versioned (a schema byte) so future changes don't misread old data.
- Vibration pattern for Done (single long vs repeated) — pick a default, no setting.
- Wakeup 1-minute-window nudging is a rare edge; log when it triggers.
