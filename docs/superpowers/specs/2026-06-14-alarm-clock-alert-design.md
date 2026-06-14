# Countdown-timer alarm — alarm-clock-style alert + button-adjacent labels

**Status:** approved design (2026-06-14)
**Repo:** `PebbleCountdownTimer/` (private submodule of the pebble-timetracking superrepo)
**Builds on:** `2026-06-13-countdown-timer-design.md` (the "Timer-finished alarm screen" revision)

## Goal

Make the timer-finished alarm behave like the stock Pebble alarm clock and be
readable at a glance:

1. **Keep buzzing until dismissed** (not a single multi-buzz), capped for battery.
2. **Action labels next to their physical buttons**, in a big bold font, with
   clearer text (`Stop` / `+1 Min`).
3. **Button mapping that matches the stock Pebble alarm clock** (UP = snooze,
   DOWN = dismiss, BACK = snooze).

## Scope

Touches **only** `src/c/main.c` — the full-screen alarm window (`s_alarm_*`), its
click handlers, and the vibration. No changes to the timer list, config (Clay/TS),
message keys, persistence, or `timer_calc`/`timer_store`. No new message keys, so
the positional message-key-ID drift caveat does not apply.

## Reference: stock Pebble alarm clock

From `PebbleOS/src/fw/popups/alarm_popup.c` (the authoritative firmware source):

- **Buttons** (`prv_click_provider`): `UP` = Snooze, `DOWN` = Dismiss,
  `BACK` = Snooze (same as UP); `SELECT` unused. Icons sit in an ActionBarLayer
  aligned to UP/DOWN.
- **Vibration** (`prv_start_vibes` / `prv_vibe_kernel_main_cb`): repeats the vibe
  pattern for `VIBE_DURATION = 10 min`, then stops **and auto-dismisses**.

We follow the button mapping and the 10-min cap, but **keep the alarm screen up**
after the cap instead of auto-dismissing (explicit user choice), and use big bold
**text** labels instead of icons (explicit user request).

## Behavior

### Buttons

| Button | Action | Handler |
|---|---|---|
| **UP** (top) | snooze: extend the finished timer by 1 min, re-arm, dismiss alarm | `alarm_add_minute` (existing) |
| **DOWN** (bottom) | stop: reset the finished timer, dismiss alarm | `alarm_select` (existing, re-bound from SELECT) |
| **BACK** | same as UP (snooze) — matches stock | `alarm_add_minute` |
| **SELECT** | unused (not subscribed) | — |

`alarm_click_config` is updated to subscribe `BUTTON_ID_UP`, `BUTTON_ID_DOWN`, and
`BUTTON_ID_BACK` accordingly (subscribing BACK overrides the default
window-pop). The handler bodies are unchanged — only which physical button invokes
each is changed, plus BACK now snoozes.

### Layout (alarm window)

- Background `GColorRed`, text `GColorWhite` (unchanged).
- **Title** (`s_alarm_title`): timer name, or `tc_format_remaining(duration)` if
  unnamed — big bold, centered, in the **middle band** (the UP/DOWN labels free up
  the centre). Word-wrap kept.
- **`+N more`**: when several timers finish together (`count > 1`), a small line
  (`GOTHIC_18`) under the title. The old multi-line `"Time's up\nUp = …\nSelect =
  …"` hint (`s_alarm_sub`) is removed/repurposed — the button labels replace it.
- **`+1 Min` label**: `GOTHIC_28_BOLD`, right-aligned, vertically centered on the
  **UP** button row (top ≈ 28 % of screen height).
- **`Stop` label**: `GOTHIC_28_BOLD`, right-aligned, vertically centered on the
  **DOWN** button row (bottom ≈ 72 % of screen height).
- Button-row Y positions are derived from `layer_get_bounds(root).size.h` (a
  fraction, not a hard-coded pixel) so the same code lands correctly on 144 px
  (diorite) and 200 px (emery / Pebble Time 2). Exact fractions/insets to be tuned
  against screenshots in the plan; ≈28 % / ≈72 % are the starting points.
- Label widths/x are chosen so the right-aligned labels don't overlap the centered
  title at the title's vertical extent (title band sits between the two label rows).

### Vibration — repeat until dismissed, capped

- Replace the single `vibes_enqueue_custom_pattern` call with a **repeating
  `AppTimer`** (`s_alarm_buzz_timer`): each fire calls the existing
  `alarm_vibrate()` then re-registers itself after `ALARM_BUZZ_INTERVAL_MS`
  (≈ pattern length + a short gap, ~3–5 s).
- **Cap:** record the start time (`s_alarm_buzz_start_s = now_s()`). When
  `now_s() - s_alarm_buzz_start_s >= ALARM_BUZZ_MAX_S` (`600`, 10 min), stop
  re-registering (no more buzzes) but **leave the alarm window up**.
- **Start/restart:** `trigger_alarm()` starts the loop and resets
  `s_alarm_buzz_start_s`. If the alarm is already showing and another timer
  finishes (the in-place text-refresh path), the cap restarts too.
- **Stop:** dismissing via Stop (`alarm_select`) or +1 Min/BACK
  (`alarm_add_minute`) removes the window; `alarm_window_unload` cancels
  `s_alarm_buzz_timer` (and `vibes_cancel()`), so no buzz fires after dismissal.
  Cancelling in `unload` covers every dismissal path centrally.
- The buzz `AppTimer` runs only while the app is foregrounded — exactly while the
  alarm is visible (the alarm is pushed when a timer expires in the foreground or
  on wakeup launch, both of which have the app running). If the app is force-closed
  the buzzing stops; acceptable.

## Constants

- `ALARM_BUZZ_INTERVAL_MS` — re-buzz cadence (~3000–5000; tune so buzzes feel
  continuous without overlapping the previous pattern).
- `ALARM_BUZZ_MAX_S` = `600` — 10-min cap (matches stock `VIBE_DURATION`).

## C gotchas to respect (from superrepo CLAUDE.md)

- No `strtol`/`strtod`/`atol` (not exported by Core Devices firmware) — none needed
  here.
- No large stack locals in handlers — none added (only a couple of `static`
  scalars: `s_alarm_buzz_timer`, `s_alarm_buzz_start_s`).
- Outbox sent/failed handlers already registered — unchanged.

## Testing / verification

- No new pure logic, so no new `timer_calc` unit tests. Existing
  `tests/test_timer_calc.c` + JS tests must still pass (`npm test`).
- **emery + diorite emulator screenshots** per the superrepo CLAUDE.md headless
  recipe: temporarily seed a `TS_DONE`/just-expired timer so `trigger_alarm` shows
  the alarm screen without a phone, build/install/screenshot on both boards,
  **surface every screenshot to the user**, then revert the seed. Confirm: `+1 Min`
  reads cleanly next to UP, `Stop` next to DOWN, title centered and legible, no
  overlap, on both 144 px and 200 px.
- **Continuous vibration + 10-min cap** can only be confirmed on the real Core
  Devices watch (the emulator delivers no real haptics); verify by ear/feel and
  that Stop/+1 Min/BACK each silence it immediately.

## Open items

- Exact button-row Y fractions and label widths — tune against screenshots.
- `ALARM_BUZZ_INTERVAL_MS` value — tune on hardware so it feels like a continuous
  alarm without gaps that read as "stopped".
