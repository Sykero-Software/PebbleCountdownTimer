# Alarm-clock-style alert + button-adjacent labels — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the countdown-timer finished-alarm behave like the stock Pebble alarm clock — keep buzzing (capped at 10 min) until dismissed, with big bold action labels (`+1 Min` / `Stop`) sitting next to their physical buttons.

**Architecture:** All changes live in `PebbleCountdownTimer/src/c/main.c`, in the full-screen alarm window (`s_alarm_*`), its click-config provider, and its vibration. A repeating `AppTimer` drives the buzz loop; the alarm `TextLayer`s are repositioned/relabelled and two new label layers are added. No changes to `timer_calc`/`timer_store`, persistence, message keys, or the Clay/TS config side.

**Tech Stack:** C, Pebble SDK 3 (`pebble.h`: `AppTimer`, `TextLayer`, `vibes_*`, `window_single_click_subscribe`). Verification via the headless emulator screenshot recipe in the superrepo `CLAUDE.md`.

**Why no unit tests:** the alarm screen, button wiring, and haptics are on-watch UI with no pure-logic surface, so there is nothing to add to `tests/test_timer_calc.c`. The TDD loop here is: existing tests stay green + emery/diorite screenshots + a real-watch buzz/dismiss check. Each task still ends in a build + commit.

**Reference for behavior:** `PebbleOS/src/fw/popups/alarm_popup.c` — stock alarm maps UP=Snooze, DOWN=Dismiss, BACK=Snooze, buzzes for `VIBE_DURATION` (10 min). We match the button map and cap; we keep the screen up after the cap instead of auto-dismissing (per the spec).

**Spec:** `docs/superpowers/specs/2026-06-14-alarm-clock-alert-design.md`

---

## Task 1: Repeating, capped buzz loop

Replace the single-shot vibration with a repeating `AppTimer` that buzzes every few seconds until a 10-minute cap, then stops buzzing but leaves the screen up. Dismissal cancels it centrally in the window unload.

**Files:**
- Modify: `PebbleCountdownTimer/src/c/main.c`

- [ ] **Step 1: Add buzz-loop state and constants**

In `main.c`, just after the existing alarm globals (after `static char s_alarm_sub_buf[48];`, around line 17), add:

```c
// Repeating "alarm clock" buzz: re-fire alarm_vibrate() on a timer until the
// user dismisses, capped at ALARM_BUZZ_MAX_S so an unattended watch stops
// buzzing (stock Pebble alarm caps at 10 min; see PebbleOS alarm_popup.c).
#define ALARM_BUZZ_INTERVAL_MS 4000   // ~pattern length (2.8s) + a short gap
#define ALARM_BUZZ_MAX_S       600    // 10 min, then stop buzzing (screen stays)
static AppTimer *s_alarm_buzz_timer;
static int64_t   s_alarm_buzz_start_s;
```

- [ ] **Step 2: Add the buzz callback + start/stop helpers**

Insert these immediately after `alarm_vibrate()` (after its closing brace, around line 77):

```c
// Re-buzz on a repeating timer until the 10-min cap; then stop scheduling but
// leave the alarm window up (the user must press Stop/+1 Min to dismiss).
static void alarm_buzz_cb(void *ctx) {
  s_alarm_buzz_timer = NULL;
  if (now_s() - s_alarm_buzz_start_s >= ALARM_BUZZ_MAX_S) { return; }
  alarm_vibrate();
  s_alarm_buzz_timer = app_timer_register(ALARM_BUZZ_INTERVAL_MS, alarm_buzz_cb, NULL);
}

static void alarm_buzz_start(void) {
  if (s_alarm_buzz_timer) { app_timer_cancel(s_alarm_buzz_timer); s_alarm_buzz_timer = NULL; }
  s_alarm_buzz_start_s = now_s();
  alarm_vibrate();   // buzz now
  s_alarm_buzz_timer = app_timer_register(ALARM_BUZZ_INTERVAL_MS, alarm_buzz_cb, NULL);
}

static void alarm_buzz_stop(void) {
  if (s_alarm_buzz_timer) { app_timer_cancel(s_alarm_buzz_timer); s_alarm_buzz_timer = NULL; }
  vibes_cancel();
}
```

- [ ] **Step 3: Drive the loop from trigger_alarm**

In `trigger_alarm()`, replace the single call `alarm_vibrate();` (currently around line 149, right after `light_enable_interaction();`) with:

```c
  alarm_buzz_start();   // repeating buzz until dismissed (cap restarts on each trigger)
```

- [ ] **Step 4: Cancel the loop on dismissal**

In `alarm_window_unload()` (around line 125), add `alarm_buzz_stop();` as the first line so EVERY dismissal path (Stop, +1 Min, Back) stops the buzzing:

```c
static void alarm_window_unload(Window *w) {
  alarm_buzz_stop();
  text_layer_destroy(s_alarm_title); s_alarm_title = NULL;
  text_layer_destroy(s_alarm_sub); s_alarm_sub = NULL;
}
```

(Two more `text_layer_destroy` lines are added here in Task 3 — that task shows the full final body.)

- [ ] **Step 5: Build to verify it compiles**

Run:
```bash
cd /home/dev/pebble-timetracking/PebbleCountdownTimer && \
  PEBBLE_QEMU_PATH=~/.local/bin/qemu-pebble-headless pebble build 2>&1 | tail -20
```
Expected: `Build Successful` (or `'waf' finished successfully`), no errors. `alarm_buzz_cb` references `now_s()`/`alarm_vibrate()` which are defined above it — no forward-declaration needed.

- [ ] **Step 6: Commit**

```bash
cd /home/dev/pebble-timetracking/PebbleCountdownTimer && \
  git add src/c/main.c && \
  git commit -m "alarm: repeat buzz until dismissed, capped at 10 min"
```

---

## Task 2: Re-map alarm buttons to match the stock alarm clock

UP and BACK snooze (+1 Min); DOWN stops. SELECT becomes unused.

**Files:**
- Modify: `PebbleCountdownTimer/src/c/main.c`

- [ ] **Step 1: Re-bind the click handlers**

Replace the body of `alarm_click_config()` (currently lines ~97–100, `SELECT`→`alarm_select`, `UP`→`alarm_add_minute`) with:

```c
static void alarm_click_config(void *ctx) {
  window_single_click_subscribe(BUTTON_ID_DOWN, alarm_select);      // Stop: reset + dismiss
  window_single_click_subscribe(BUTTON_ID_UP, alarm_add_minute);    // +1 Min: snooze + dismiss
  window_single_click_subscribe(BUTTON_ID_BACK, alarm_add_minute);  // Back = snooze (matches stock)
}
```

The handler bodies `alarm_select` and `alarm_add_minute` are unchanged — only which physical button invokes each changes, and BACK now snoozes (subscribing BACK overrides the default window-pop).

- [ ] **Step 2: Build to verify it compiles**

Run:
```bash
cd /home/dev/pebble-timetracking/PebbleCountdownTimer && \
  PEBBLE_QEMU_PATH=~/.local/bin/qemu-pebble-headless pebble build 2>&1 | tail -5
```
Expected: build succeeds.

- [ ] **Step 3: Commit**

```bash
cd /home/dev/pebble-timetracking/PebbleCountdownTimer && \
  git add src/c/main.c && \
  git commit -m "alarm: map buttons like stock alarm (UP/BACK=+1Min, DOWN=Stop)"
```

---

## Task 3: Button-adjacent bold labels + centered title

Add two big-bold right-aligned label layers anchored to the UP (top) and DOWN (bottom) buttons, move the title to the centre, and reduce the sub line to a small top-centre `+N more` count.

**Files:**
- Modify: `PebbleCountdownTimer/src/c/main.c`

- [ ] **Step 1: Add the two label-layer globals**

Next to the other alarm `TextLayer` globals (after `static TextLayer *s_alarm_sub;`, around line 14), add:

```c
static TextLayer *s_alarm_lbl_up;    // "+1 Min" next to the UP button
static TextLayer *s_alarm_lbl_down;  // "Stop"  next to the DOWN button
```

- [ ] **Step 2: Rewrite alarm_window_load with the new layout**

Replace the entire `alarm_window_load()` function (currently ~lines 102–123) with:

```c
static void alarm_window_load(Window *w) {
  window_set_background_color(w, GColorRed);
  Layer *root = window_get_root_layer(w);
  GRect b = layer_get_bounds(root);
  const int h = b.size.h, wd = b.size.w;

  // "+N more" — small, top-centre (free space; the UP label is right-aligned).
  s_alarm_sub = text_layer_create(GRect(4, 2, wd - 8, 22));
  text_layer_set_background_color(s_alarm_sub, GColorClear);
  text_layer_set_text_color(s_alarm_sub, GColorWhite);
  text_layer_set_font(s_alarm_sub, fonts_get_system_font(FONT_KEY_GOTHIC_18));
  text_layer_set_text_alignment(s_alarm_sub, GTextAlignmentCenter);
  text_layer_set_text(s_alarm_sub, s_alarm_sub_buf);
  layer_add_child(root, text_layer_get_layer(s_alarm_sub));

  // "+1 Min" — big bold, right-aligned, vertically by the UP button (~22% h).
  s_alarm_lbl_up = text_layer_create(GRect(0, h * 22 / 100 - 16, wd - 6, 34));
  text_layer_set_background_color(s_alarm_lbl_up, GColorClear);
  text_layer_set_text_color(s_alarm_lbl_up, GColorWhite);
  text_layer_set_font(s_alarm_lbl_up, fonts_get_system_font(FONT_KEY_GOTHIC_28_BOLD));
  text_layer_set_text_alignment(s_alarm_lbl_up, GTextAlignmentRight);
  text_layer_set_text(s_alarm_lbl_up, "+1 Min");
  layer_add_child(root, text_layer_get_layer(s_alarm_lbl_up));

  // Title — big bold, centred middle band (timer name, or time if unnamed).
  s_alarm_title = text_layer_create(GRect(4, h / 2 - 32, wd - 8, 64));
  text_layer_set_background_color(s_alarm_title, GColorClear);
  text_layer_set_text_color(s_alarm_title, GColorWhite);
  text_layer_set_font(s_alarm_title, fonts_get_system_font(FONT_KEY_GOTHIC_28_BOLD));
  text_layer_set_text_alignment(s_alarm_title, GTextAlignmentCenter);
  text_layer_set_overflow_mode(s_alarm_title, GTextOverflowModeWordWrap);
  text_layer_set_text(s_alarm_title, s_alarm_title_buf);
  layer_add_child(root, text_layer_get_layer(s_alarm_title));

  // "Stop" — big bold, right-aligned, vertically by the DOWN button (~78% h).
  s_alarm_lbl_down = text_layer_create(GRect(0, h * 78 / 100 - 18, wd - 6, 34));
  text_layer_set_background_color(s_alarm_lbl_down, GColorClear);
  text_layer_set_text_color(s_alarm_lbl_down, GColorWhite);
  text_layer_set_font(s_alarm_lbl_down, fonts_get_system_font(FONT_KEY_GOTHIC_28_BOLD));
  text_layer_set_text_alignment(s_alarm_lbl_down, GTextAlignmentRight);
  text_layer_set_text(s_alarm_lbl_down, "Stop");
  layer_add_child(root, text_layer_get_layer(s_alarm_lbl_down));
}
```

- [ ] **Step 3: Destroy the two new layers in unload**

Replace `alarm_window_unload()` (as left after Task 1 Step 4) with the full final body:

```c
static void alarm_window_unload(Window *w) {
  alarm_buzz_stop();
  text_layer_destroy(s_alarm_title); s_alarm_title = NULL;
  text_layer_destroy(s_alarm_sub); s_alarm_sub = NULL;
  text_layer_destroy(s_alarm_lbl_up); s_alarm_lbl_up = NULL;
  text_layer_destroy(s_alarm_lbl_down); s_alarm_lbl_down = NULL;
}
```

- [ ] **Step 4: Make the sub line carry only the "+N more" count**

In `trigger_alarm()`, replace the `if (count > 1) … else …` block that fills `s_alarm_sub_buf` (currently ~lines 141–147, the `"Time's up\nUp = +1 min\nSelect = reset"` strings) with:

```c
  if (count > 1) {
    snprintf(s_alarm_sub_buf, sizeof(s_alarm_sub_buf), "+%d more", count - 1);
  } else {
    s_alarm_sub_buf[0] = '\0';
  }
```

The existing in-place refresh branch (`if (window_stack_get_top_window() == s_alarm_window)`) already re-sets `s_alarm_title`/`s_alarm_sub` text; the label layers are constant so they need no refresh.

- [ ] **Step 5: Build to verify it compiles**

Run:
```bash
cd /home/dev/pebble-timetracking/PebbleCountdownTimer && \
  PEBBLE_QEMU_PATH=~/.local/bin/qemu-pebble-headless pebble build 2>&1 | tail -8
```
Expected: build succeeds.

- [ ] **Step 6: Commit**

```bash
cd /home/dev/pebble-timetracking/PebbleCountdownTimer && \
  git add src/c/main.c && \
  git commit -m "alarm: bold +1 Min / Stop labels next to UP/DOWN, centered title"
```

---

## Task 4: Screenshot verification on emery + diorite

Temporarily force the alarm screen at launch (gated on the existing `SCREENSHOT_FIXTURES` env define), screenshot on both boards, surface to the user, then revert the temporary seed.

**Files:**
- Modify (temporarily, reverted in Step 6): `PebbleCountdownTimer/src/c/main.c`

- [ ] **Step 1: Add a temporary alarm seed at the end of init()**

The `SCREENSHOT_FIXTURES` branch in `init()` already seeds 3 demo timers (`Egg`/`Tea`/`Laundry`). At the very end of `init()` — after the `if (by_wakeup && fired) { trigger_alarm(...); }` line (~line 378) and before the closing brace — add:

```c
#ifdef SCREENSHOT_FIXTURES
  trigger_alarm(0, 2);   // TEMP: force the alarm screen for screenshots (count=2 -> "+1 more")
#endif
```

- [ ] **Step 2: Boot the emery emulator (the user's real board is a Pebble Time 2 = emery)**

Run (cold-boot once, then reuse — see the CLAUDE.md headless recipe):
```bash
cd /home/dev/pebble-timetracking && scripts/pebble-emu-boot.sh emery
```
Expected: the helper reports a successful boot (typically attempt 1–2).

- [ ] **Step 3: Build with fixtures, install, screenshot (emery)**

Run as **separate** calls (do not `&&`-chain install+screenshot — see CLAUDE.md):
```bash
cd /home/dev/pebble-timetracking/PebbleCountdownTimer && \
  SCREENSHOT_FIXTURES=1 PEBBLE_QEMU_PATH=~/.local/bin/qemu-pebble-headless pebble build 2>&1 | tail -3
```
then:
```bash
cd /home/dev/pebble-timetracking/PebbleCountdownTimer && \
  PEBBLE_QEMU_PATH=~/.local/bin/qemu-pebble-headless pebble install --emulator emery 2>&1 | tail -3
```
then:
```bash
cd /home/dev/pebble-timetracking/PebbleCountdownTimer && \
  PEBBLE_QEMU_PATH=~/.local/bin/qemu-pebble-headless pebble screenshot --no-open alarm-emery.png 2>&1 | tail -3
```
Expected: `alarm-emery.png` written, showing the red alarm with `+1 Min` top-right, `Stop` bottom-right, the timer name (`Egg`) centred, `+1 more` top-centre.

- [ ] **Step 4: Surface the emery screenshot to the user**

Use the SendUserFile tool to send `PebbleCountdownTimer/alarm-emery.png` (status: proactive). Per CLAUDE.md, always surface every screenshot.

- [ ] **Step 5: Repeat for diorite (144 px) and surface it**

```bash
cd /home/dev/pebble-timetracking && scripts/pebble-emu-boot.sh diorite
```
then build is already done; install + screenshot:
```bash
cd /home/dev/pebble-timetracking/PebbleCountdownTimer && \
  PEBBLE_QEMU_PATH=~/.local/bin/qemu-pebble-headless pebble install --emulator diorite 2>&1 | tail -3
```
```bash
cd /home/dev/pebble-timetracking/PebbleCountdownTimer && \
  PEBBLE_QEMU_PATH=~/.local/bin/qemu-pebble-headless pebble screenshot --no-open alarm-diorite.png 2>&1 | tail -3
```
Surface `alarm-diorite.png` to the user with SendUserFile. **Decision point:** if labels overlap the title, are clipped, or read poorly on either board, tune the `GRect` Y fractions (`h*22/100`, `h*78/100`), heights, or the title band in Task 3 Step 2 and re-screenshot before continuing. The exact fractions are an explicit open item in the spec.

- [ ] **Step 6: Revert the temporary seed**

Remove the `#ifdef SCREENSHOT_FIXTURES trigger_alarm(0, 2); #endif` block added in Step 1. Verify it's gone:
```bash
cd /home/dev/pebble-timetracking/PebbleCountdownTimer && \
  grep -n "TEMP: force the alarm" src/c/main.c
```
Expected: no output (line removed).

- [ ] **Step 7: Rebuild clean (no fixtures) and confirm green tests**

```bash
cd /home/dev/pebble-timetracking/PebbleCountdownTimer && \
  PEBBLE_QEMU_PATH=~/.local/bin/qemu-pebble-headless pebble build 2>&1 | tail -3 && \
  npm test 2>&1 | tail -15
```
Expected: build succeeds; `node --test` reports all existing JS tests passing (the C/UI changes don't touch JS, so they stay green).

- [ ] **Step 8: Remove screenshot artifacts and commit any layout tuning**

The `alarm-*.png` files are build artifacts; delete them (they are not committed):
```bash
cd /home/dev/pebble-timetracking/PebbleCountdownTimer && rm -f alarm-emery.png alarm-diorite.png
```
If Task 3 geometry was tuned in Step 5, commit it:
```bash
cd /home/dev/pebble-timetracking/PebbleCountdownTimer && \
  git add src/c/main.c && \
  git commit -m "alarm: tune label/title geometry from screenshots" || echo "no tuning changes to commit"
```

---

## Task 5: Bump the superrepo gitlink + hardware verification

**Files:**
- Modify: superrepo gitlink for `PebbleCountdownTimer`

- [ ] **Step 1: Bump the gitlink in the superrepo**

Per CLAUDE.md, commit the submodule pointer right after submodule commits:
```bash
cd /home/dev/pebble-timetracking && \
  git add PebbleCountdownTimer && \
  git commit -m "Bump PebbleCountdownTimer: alarm-clock-style alert + button labels"
```

- [ ] **Step 2: Install on the real watch (CloudPebble) for haptics verification**

The emulator delivers no real vibration, so the continuous-buzz behavior must be checked on hardware. Requires `pebble login` done + Core app signed in (see CLAUDE.md):
```bash
cd /home/dev/pebble-timetracking/PebbleCountdownTimer && \
  PEBBLE_QEMU_PATH=~/.local/bin/qemu-pebble-headless pebble build && \
  pebble install --cloudpebble 2>&1 | tail -5
```
(If `gie net loose` / repebble.com allowlist is needed for the relay, surface that to the user rather than guessing.)

- [ ] **Step 3: Manual on-watch checklist (report results to the user)**

Configure a short timer (e.g. 5 s) on the phone, let it expire, and confirm:
- The alarm keeps buzzing repeatedly (not a single burst).
- **DOWN** = `Stop` resets the timer and dismisses; buzzing stops immediately.
- **UP** = `+1 Min` snoozes (timer runs ~1 more min, alarm re-fires) and dismisses; buzzing stops.
- **BACK** snoozes like UP; buzzing stops.
- Labels are readable next to their buttons; title legible.
- (Optional/patience) buzzing stops on its own after ~10 min but the screen stays until a button is pressed.

Report pass/fail per item to the user. No `PC: <low addr>` hard fault on the watch when the alarm fires (would indicate a forbidden syscall — none introduced here).

---

## Self-review notes

- **Spec coverage:** capped repeat buzz (Task 1) ✓; keep-screen-after-cap (Task 1 Step 2, early-return without dismiss) ✓; button map UP/BACK=+1Min, DOWN=Stop, SELECT unused (Task 2) ✓; bold button-adjacent labels + centered title + `+N more` (Task 3) ✓; screenshot verification both boards, surfaced + revert seed (Task 4) ✓; gitlink bump + hardware haptics check (Task 5) ✓.
- **No new message keys / no `strtol` / no big stack locals** — respected (only two `static` scalars added).
- **Type/name consistency:** `alarm_buzz_start`/`alarm_buzz_stop`/`alarm_buzz_cb`, `s_alarm_buzz_timer`, `s_alarm_buzz_start_s`, `s_alarm_lbl_up`/`s_alarm_lbl_down` used consistently across tasks.
