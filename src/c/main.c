// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Tuomas Airaksinen
#include <pebble.h>
#include "timer_calc.h"
#include "timer_store.h"

static Window *s_window;
static MenuLayer *s_menu;
static AppTimer *s_tick;

// Full-screen alarm shown when a timer reaches zero.
static Window *s_alarm_window;
static TextLayer *s_alarm_title;
static TextLayer *s_alarm_sub;
static TextLayer *s_alarm_lbl_up;    // "+1 Min" next to the UP button
static TextLayer *s_alarm_lbl_down;  // "Stop"  next to the DOWN button
static int s_alarm_idx = -1;                 // config index the alarm screen is for
static char s_alarm_title_buf[NAME_LEN + 1]; // big name (or time if unnamed)
static char s_alarm_sub_buf[48];

// Repeating "alarm clock" buzz: re-fire alarm_vibrate() on a timer until the
// user dismisses, capped at ALARM_BUZZ_MAX_S so an unattended watch stops
// buzzing (stock Pebble alarm caps at 10 min; see PebbleOS alarm_popup.c).
#define ALARM_BUZZ_INTERVAL_MS 4000   // ~pattern length (2.8s) + a short gap
#define ALARM_BUZZ_MAX_S       600    // 10 min, then stop buzzing (screen stays)
static AppTimer *s_alarm_buzz_timer;
static int64_t   s_alarm_buzz_start_s;

static Timer s_timers[MAX_TIMERS];
static int s_count = 0;
static int s_order[MAX_TIMERS];   // display order, rebuilt on reload per s_sort
static SortMode s_sort = SORT_MRU;
static int s_last_fired_idx = -1; // first timer that newly expired in the latest sweep
static bool s_auto_return = false; // config: pop to watchface after a start/resume
static bool s_running_first = true; // config: float RUNNING timers to the top

// ---- per-timer detail window: live-time header + Pause/Stop/+N actions ----
static Window *s_detail_window;
static MenuLayer *s_detail_menu;
static int s_detail_idx = -1;   // config index the detail window is showing

// ---- transient "Started" confirmation shown ~1.1s before auto-return closes the app ----
static Window  *s_confirm_window;
static Layer   *s_confirm_layer;
static AppTimer *s_confirm_timer;
static char s_confirm_name[NAME_LEN + 1];
static char s_confirm_time[16];
static bool s_confirm_named;

static int64_t now_s(void) { return (int64_t)time(NULL); }

// ---- wakeup: keep exactly ONE armed for the soonest running end_time ----
static void rearm_wakeup(void) {
  int32_t old = store_load_wakeup_id();
  if (old >= 0) { wakeup_cancel(old); store_save_wakeup_id(-1); }
  int64_t soon;
  if (tc_soonest_end(s_timers, s_count, &soon)) {
    time_t when = (time_t)soon;
    time_t nowt = time(NULL);
    if (when <= nowt) { when = nowt + 1; }
    // wakeup needs >=1 min separation from other apps; nudge forward on failure.
    for (int attempt = 0; attempt < 4; attempt++) {
      WakeupId id = wakeup_schedule(when, 0, true);
      if (id >= 0) { store_save_wakeup_id(id); return; }
      when += 60;   // E_RANGE / slot taken: try the next minute
    }
    APP_LOG(APP_LOG_LEVEL_WARNING, "wakeup_schedule failed after retries");
  }
}

static void persist_all(void) { store_save(s_timers, s_count); }

static void rebuild_order(void) { tc_display_order(s_timers, s_count, s_sort, now_s(), s_order, s_running_first); }

static void reload_ui(void) {
  rebuild_order();
  if (s_menu) { menu_layer_reload_data(s_menu); }
}

static void ensure_ticking(void);   // defined below; used by the alarm handlers
static void open_detail_window(int timer_idx);  // defined below; used by the alarm snooze

// Mark every expired RUNNING timer DONE. Returns the count that NEWLY expired and
// sets s_last_fired_idx to the first of them (drives the alarm screen). No UI here.
static int sweep_expiries(void) {
  int fired = 0;
  int64_t now = now_s();
  for (int i = 0; i < s_count; i++) {
    if (tc_check_expiry(&s_timers[i], now)) {
      if (fired == 0) { s_last_fired_idx = i; }
      fired++;
    }
  }
  return fired;
}

// ---- full-screen alarm when a timer finishes ----
static void alarm_vibrate(void) {
  // a bit longer than a single long pulse: several long buzzes
  static const uint32_t segs[] = {500, 200, 500, 200, 500, 200, 700};
  VibePattern pat = { .durations = segs, .num_segments = ARRAY_LENGTH(segs) };
  vibes_enqueue_custom_pattern(pat);
}

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

static void alarm_stop(ClickRecognizerRef rec, void *ctx) {
  // Stop: reset the finished timer directly from the alarm, then close the app
  // (-> watchface). The alarm often fires while the app is closed (wakeup-launched),
  // so after dismissing the user wants the watchface, not to be left in the app.
  if (s_alarm_idx >= 0 && s_alarm_idx < s_count) {
    tc_reset(&s_timers[s_alarm_idx], now_s());
    persist_all(); rearm_wakeup(); reload_ui();
  }
  window_stack_pop_all(true);
}

static void alarm_add_minute(ClickRecognizerRef rec, void *ctx) {
  // Snooze: run the finished timer for 1 more minute, dismiss the alarm, and land on
  // that timer's own detail window. Push the detail window over the alarm first, then
  // silently drop the alarm from beneath it (no flash back to the list).
  int idx = s_alarm_idx;
  if (idx >= 0 && idx < s_count) {
    tc_extend(&s_timers[idx], 60, now_s());
    persist_all(); rearm_wakeup(); ensure_ticking(); reload_ui();
    open_detail_window(idx);
    window_stack_remove(s_alarm_window, false);
  } else {
    window_stack_remove(s_alarm_window, true);
  }
}

static void alarm_click_config(void *ctx) {
  window_single_click_subscribe(BUTTON_ID_DOWN, alarm_stop);        // Stop: reset + dismiss
  window_single_click_subscribe(BUTTON_ID_UP, alarm_add_minute);    // +1 Min: snooze + dismiss
  window_single_click_subscribe(BUTTON_ID_BACK, alarm_add_minute);  // Back = snooze (matches stock)
}

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

  // Title — large bold, centred middle band (timer name, or time if unnamed).
  s_alarm_title = text_layer_create(GRect(2, h / 2 - 36, wd - 4, 72));
  text_layer_set_background_color(s_alarm_title, GColorClear);
  text_layer_set_text_color(s_alarm_title, GColorWhite);
  text_layer_set_font(s_alarm_title, fonts_get_system_font(FONT_KEY_BITHAM_42_BOLD));
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

static void alarm_window_unload(Window *w) {
  alarm_buzz_stop();
  text_layer_destroy(s_alarm_title); s_alarm_title = NULL;
  text_layer_destroy(s_alarm_sub); s_alarm_sub = NULL;
  text_layer_destroy(s_alarm_lbl_up); s_alarm_lbl_up = NULL;
  text_layer_destroy(s_alarm_lbl_down); s_alarm_lbl_down = NULL;
}

// Show the alarm for timer `idx` (first of `count` that just finished): big name,
// long vibration, backlight on briefly; Select resets it.
static void trigger_alarm(int idx, int count) {
  if (idx < 0 || idx >= s_count) { return; }
  s_alarm_idx = idx;
  Timer *t = &s_timers[idx];
  if (t->name[0]) {
    snprintf(s_alarm_title_buf, sizeof(s_alarm_title_buf), "%s", t->name);
  } else {
    tc_format_remaining(s_alarm_title_buf, sizeof(s_alarm_title_buf), t->duration);
  }
  if (count > 1) {
    snprintf(s_alarm_sub_buf, sizeof(s_alarm_sub_buf), "+%d more", count - 1);
  } else {
    s_alarm_sub_buf[0] = '\0';
  }
  light_enable_interaction();   // backlight on for the standard brief window
  alarm_buzz_start();   // repeating buzz until dismissed (cap restarts on each trigger)
  if (!s_alarm_window) {
    s_alarm_window = window_create();
    window_set_window_handlers(s_alarm_window, (WindowHandlers){
      .load = alarm_window_load, .unload = alarm_window_unload });
    window_set_click_config_provider(s_alarm_window, alarm_click_config);
  }
  if (window_stack_get_top_window() == s_alarm_window) {
    // already showing (another timer finished): refresh the text in place
    if (s_alarm_title) { text_layer_set_text(s_alarm_title, s_alarm_title_buf); }
    if (s_alarm_sub) { text_layer_set_text(s_alarm_sub, s_alarm_sub_buf); }
  } else {
    window_stack_push(s_alarm_window, true);
  }
}

// ---- 1s foreground tick: refresh running rows + catch foreground expiries ----
static void tick_cb(void *ctx) {
  int fired = sweep_expiries();
  bool running = false;
  for (int i = 0; i < s_count; i++) { if (s_timers[i].state == TS_RUNNING) { running = true; } }
  reload_ui();
  if (s_detail_menu && window_stack_get_top_window() == s_detail_window) {
    menu_layer_reload_data(s_detail_menu);   // retick the live time header
  }
  if (fired) { persist_all(); rearm_wakeup(); trigger_alarm(s_last_fired_idx, fired); }
  s_tick = running ? app_timer_register(1000, tick_cb, NULL) : NULL;
}

static void ensure_ticking(void) {
  if (s_tick) { return; }
  for (int i = 0; i < s_count; i++) {
    if (s_timers[i].state == TS_RUNNING) { s_tick = app_timer_register(1000, tick_cb, NULL); return; }
  }
}

// ---- per-timer detail window: live-time header + Pause/Stop/+N actions ----

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

// Startable = the detail window's timer can be started fresh (idle or done); these
// states offer "Save as new & start" instead of "Stop".
static bool detail_startable(void) {
  if (s_detail_idx < 0 || s_detail_idx >= s_count) { return false; }
  TimerState st = s_timers[s_detail_idx].state;
  return st == TS_IDLE || st == TS_DONE;
}

static uint16_t dl_num_rows(MenuLayer *ml, uint16_t section, void *ctx) {
  return 4;   // row0 primary, row1 secondary, +1 min, -1 min
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
    graphics_draw_text(gctx, t->name, f, GRect(4, 2, b.size.w - 80, 22),
      GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
    graphics_draw_text(gctx, rem, f, GRect(4, 2, b.size.w - 8, 22),
      GTextOverflowModeTrailingEllipsis, GTextAlignmentRight, NULL);
  } else {
    graphics_draw_text(gctx, rem, f, GRect(4, 2, b.size.w - 8, 22),
      GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
  }
}

static const char *dl_row_label(int row, TimerState st) {
  switch (row) {
    case 0: return (st == TS_RUNNING) ? "Pause" : "Start";   // paused/idle/done -> Start(=resume/start)
    case 1: return (st == TS_IDLE || st == TS_DONE) ? "Save as new & start" : "Stop";
    case 2: return "+1 min";
    case 3: return "-1 min";
    default: return "";
  }
}

static void dl_draw_row(GContext *gctx, const Layer *cell, MenuIndex *ci, void *ctx) {
  TimerState st = (s_detail_idx >= 0 && s_detail_idx < s_count)
                  ? s_timers[s_detail_idx].state : TS_IDLE;
  GRect b = layer_get_bounds(cell);
  graphics_draw_text(gctx, dl_row_label(ci->row, st),
    fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
    GRect(6, 1, b.size.w - 12, b.size.h - 1),
    GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
}

static void save_as_new_and_start(int32_t secs);  // defined in Task 9

static void dl_select(MenuLayer *ml, MenuIndex *ci, void *ctx) {
  int idx = s_detail_idx;
  if (idx < 0 || idx >= s_count) { return; }
  Timer *t = &s_timers[idx];
  if (ci->row == 0) {                 // Pause / Start(resume/start) -> keep window open
    if (t->state == TS_RUNNING) { tc_pause(t, now_s()); }
    else { tc_start(t, now_s()); }
    persist_all(); rearm_wakeup(); ensure_ticking();
    reload_ui(); menu_layer_reload_data(s_detail_menu);
  } else if (ci->row == 1 && detail_startable()) {   // Save as new & start
    int32_t rem = tc_remaining_now(t, now_s());
    save_as_new_and_start(rem >= 1 ? rem : t->duration);
  } else if (ci->row == 1) {          // Stop (reset) -- running/paused
    tc_reset(t, now_s());
    persist_all(); rearm_wakeup(); reload_ui(); select_timer_row(idx);
    if (s_auto_return) { window_stack_pop_all(true); }
    else { window_stack_remove(s_detail_window, true); }
  } else {                            // row 2/3: +1 / -1 min
    int32_t secs = (ci->row == 2) ? 60 : -60;
    if (t->state == TS_RUNNING || t->state == TS_PAUSED) {
      tc_add(t, secs, now_s());
    } else {                          // idle/done: adjust `remaining` (60s floor), keep template
      int32_t r = t->remaining + secs;
      if (r < 60) { r = 60; }
      t->remaining = r;
      t->last_used = now_s();
    }
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
  // Idempotent: if it is already on the stack (e.g. it was open under the alarm when
  // a timer expired), just refresh it instead of pushing it a second time.
  if (window_stack_contains_window(s_detail_window)) {
    if (s_detail_menu) { menu_layer_reload_data(s_detail_menu); }
  } else {
    window_stack_push(s_detail_window, true);
  }
}

// ---- MenuLayer callbacks ----
static uint16_t ml_num_rows(MenuLayer *ml, uint16_t section, void *ctx) {
  return s_count == 0 ? 1 : s_count;
}
static int16_t ml_cell_height(MenuLayer *ml, MenuIndex *ci, void *ctx) { return 44; }

static const char *state_label(TimerState st) {
  switch (st) {
    case TS_RUNNING: return "Running";
    case TS_PAUSED:  return "Paused";
    case TS_DONE:    return "Done";
    default:         return "";
  }
}

static void ml_draw_row(GContext *gctx, const Layer *cell, MenuIndex *ci, void *ctx) {
  if (s_count == 0) {
    menu_cell_basic_draw(gctx, cell, "No timers", "Configure on your phone", NULL);
    return;
  }
  int idx = s_order[ci->row];
  Timer *t = &s_timers[idx];
  // Tint each row by state so running/paused/done stand out at a glance (color
  // displays only; b&w falls back to the standard white/black look). The selected
  // row uses a DARK shade of the same hue + white text so it still reads as the
  // cursor AND keeps its state colour; idle selected stays the plain black highlight.
  MenuIndex sel = menu_layer_get_selected_index(s_menu);
  bool selected = (menu_index_compare(&sel, ci) == 0);
  GColor bg, fg;
  if (selected) {
    fg = GColorWhite;
    switch (t->state) {
      case TS_RUNNING: bg = PBL_IF_COLOR_ELSE(GColorDarkGreen, GColorBlack); break;
      case TS_PAUSED:  bg = PBL_IF_COLOR_ELSE(GColorArmyGreen, GColorBlack); break;
      case TS_DONE:    bg = PBL_IF_COLOR_ELSE(GColorDarkCandyAppleRed, GColorBlack); break;
      default:         bg = GColorBlack; break;   // TS_IDLE -> standard highlight
    }
  } else {
    fg = GColorBlack;
    switch (t->state) {
      case TS_RUNNING: bg = PBL_IF_COLOR_ELSE(GColorMediumSpringGreen, GColorWhite); break;
      case TS_PAUSED:  bg = PBL_IF_COLOR_ELSE(GColorYellow, GColorWhite); break;
      case TS_DONE:    bg = PBL_IF_COLOR_ELSE(GColorSunsetOrange, GColorWhite); break;
      default:         bg = GColorWhite; break;   // TS_IDLE
    }
  }
  graphics_context_set_fill_color(gctx, bg);
  graphics_fill_rect(gctx, layer_get_bounds(cell), 0, GCornerNone);
  graphics_context_set_text_color(gctx, fg);
  char rem[16]; tc_format_remaining(rem, sizeof(rem), tc_remaining_now(t, now_s()));
  if (t->name[0]) {
    char sub[40]; snprintf(sub, sizeof(sub), "%s  %s", rem, state_label(t->state));
    menu_cell_basic_draw(gctx, cell, t->name, sub, NULL);
  } else {
    menu_cell_basic_draw(gctx, cell, rem, state_label(t->state), NULL);
  }
}

static void confirm_timer_cb(void *data) {
  s_confirm_timer = NULL;
  window_stack_pop_all(true);   // close the app -> watchface
}

static void confirm_update_proc(Layer *layer, GContext *gctx) {
  GRect b = layer_get_bounds(layer);
  int cx = b.size.w / 2;
  int cy = b.size.h / 2;
  // checkmark (two thick strokes), black on the green/white window background
  graphics_context_set_stroke_color(gctx, GColorBlack);
  graphics_context_set_stroke_width(gctx, 4);
  graphics_draw_line(gctx, GPoint(cx - 18, cy - 34), GPoint(cx - 6, cy - 22));
  graphics_draw_line(gctx, GPoint(cx - 6, cy - 22), GPoint(cx + 20, cy - 48));
  graphics_context_set_text_color(gctx, GColorBlack);
  // name (or the time, if unnamed)
  graphics_draw_text(gctx, s_confirm_name, fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD),
    GRect(4, cy - 14, b.size.w - 8, 30), GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
  // duration (only when named, to avoid showing the time twice)
  if (s_confirm_named) {
    graphics_draw_text(gctx, s_confirm_time, fonts_get_system_font(FONT_KEY_GOTHIC_18),
      GRect(4, cy + 16, b.size.w - 8, 22), GTextOverflowModeFill, GTextAlignmentCenter, NULL);
  }
  // "Started"
  graphics_draw_text(gctx, "Started", fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
    GRect(4, cy + 38, b.size.w - 8, 22), GTextOverflowModeFill, GTextAlignmentCenter, NULL);
}

static void confirm_window_load(Window *w) {
  Layer *root = window_get_root_layer(w);
  s_confirm_layer = layer_create(layer_get_bounds(root));
  layer_set_update_proc(s_confirm_layer, confirm_update_proc);
  layer_add_child(root, s_confirm_layer);
}

static void confirm_window_unload(Window *w) {
  if (s_confirm_timer) { app_timer_cancel(s_confirm_timer); s_confirm_timer = NULL; }
  if (s_confirm_layer) { layer_destroy(s_confirm_layer); s_confirm_layer = NULL; }
}

// Flash a "Started" screen for ~1.1s, then pop the whole stack (-> watchface).
// Only called on the idle one-tap start when AutoReturn is on.
static void show_start_confirmation(int idx) {
  Timer *t = &s_timers[idx];
  tc_format_remaining(s_confirm_time, sizeof(s_confirm_time), tc_remaining_now(t, now_s()));
  s_confirm_named = (t->name[0] != 0);
  if (s_confirm_named) {
    strncpy(s_confirm_name, t->name, sizeof(s_confirm_name));
  } else {
    strncpy(s_confirm_name, s_confirm_time, sizeof(s_confirm_name));
  }
  s_confirm_name[sizeof(s_confirm_name) - 1] = 0;
  if (!s_confirm_window) {
    s_confirm_window = window_create();
    window_set_background_color(s_confirm_window, PBL_IF_COLOR_ELSE(GColorMintGreen, GColorWhite));
    window_set_window_handlers(s_confirm_window,
      (WindowHandlers){ .load = confirm_window_load, .unload = confirm_window_unload });
  }
  window_stack_push(s_confirm_window, true);
  s_confirm_timer = app_timer_register(1100, confirm_timer_cb, NULL);
}

static void ml_select(MenuLayer *ml, MenuIndex *ci, void *ctx) {
  if (s_count == 0) { return; }
  int idx = s_order[ci->row];
  // An unstarted (idle) timer has only one useful action — skip the menu, just start.
  if (s_timers[idx].state == TS_IDLE) {
    tc_start(&s_timers[idx], now_s());
    persist_all(); rearm_wakeup(); ensure_ticking(); reload_ui();
    select_timer_row(idx);
    if (s_auto_return) { show_start_confirmation(idx); }   // flash, then pop to watchface
    return;
  } else {
    open_detail_window(idx);
  }
}

// Long SELECT opens the detail window for ANY timer (short SELECT still starts an
// idle timer directly). This is how an idle/done timer reaches the +/- adjust and
// the "Save as new & start" action.
static void ml_select_long(MenuLayer *ml, MenuIndex *ci, void *ctx) {
  if (s_count == 0) { return; }
  open_detail_window(s_order[ci->row]);
}

// ---- AppMessage inbox: a TimerConfig string + SortOrder int -> reconcile ----
static void inbox_received(DictionaryIterator *iter, void *ctx) {
  Tuple *sort = dict_find(iter, MESSAGE_KEY_SortOrder);
  if (sort) {
    int m = (int)sort->value->int32;
    if (m < SORT_MRU || m > SORT_LONGEST) { m = SORT_MRU; }
    s_sort = (SortMode)m;
    store_save_sort(s_sort);
  }
  Tuple *autoret = dict_find(iter, MESSAGE_KEY_AutoReturn);
  if (autoret) {
    s_auto_return = autoret->value->int32 != 0;
    store_save_autoreturn(s_auto_return);
  }
  Tuple *runfirst = dict_find(iter, MESSAGE_KEY_RunningFirst);
  if (runfirst) {
    s_running_first = runfirst->value->int32 != 0;
    store_save_runningfirst(s_running_first);
  }
  Tuple *cfg = dict_find(iter, MESSAGE_KEY_TimerConfig);
  if (cfg) {
    // static, NOT on the stack: two Timer[MAX_TIMERS] arrays are ~2 KB and would
    // overflow the Pebble app stack. The inbox handler runs on the single event
    // loop, so static is safe.
    static Timer parsed[MAX_TIMERS];
    static Timer merged[MAX_TIMERS];
    int pn = tc_parse_config(cfg->value->cstring, parsed, MAX_TIMERS);
    int mn = tc_reconcile(s_timers, s_count, parsed, pn, merged);
    memcpy(s_timers, merged, sizeof(Timer) * (size_t)mn);
    s_count = mn;
    sweep_expiries();   // mark stale expiries DONE; no alarm for a config reconcile
    persist_all(); rearm_wakeup(); ensure_ticking();
  }
  reload_ui();
}

// Outbox result handlers. These MUST be registered before sending: on hardware
// the phone ACKs our outbound Request and the SDK invokes the result callback —
// if it's NULL the app jumps to a null address and faults (the emulator never
// hits this because there's no phone to ACK).
static void outbox_sent(DictionaryIterator *iter, void *ctx) {}
static void outbox_failed(DictionaryIterator *iter, AppMessageResult reason, void *ctx) {
  APP_LOG(APP_LOG_LEVEL_WARNING, "outbox failed: %d", (int)reason);
}

// Ask the phone for the current config. A watchapp only receives AppMessages
// while running, so config saved on the phone while this app was closed never
// arrived; on launch we request it and the phone replies (see config_sync.ts).
static void request_config(void) {
  DictionaryIterator *out;
  if (app_message_outbox_begin(&out) == APP_MSG_OK) {
    dict_write_uint8(out, MESSAGE_KEY_Request, 1);
    app_message_outbox_send();
  }
}

// Tell the phone to save a new unnamed timer of `secs` seconds (appended to its
// TimerConfig + Clay store). The watch keeps the running timer locally (flagged
// custom) so it survives even if this send fails / the phone is offline.
static void send_add_timer(int32_t secs) {
  DictionaryIterator *out;
  if (app_message_outbox_begin(&out) == APP_MSG_OK) {
    dict_write_uint32(out, MESSAGE_KEY_AddTimer, (uint32_t)secs);
    app_message_outbox_send();
  }
}

// Create a NEW unnamed timer of `secs`, started now, appended at the end of the
// list (so a later config reconcile aligns the phone's appended entry to this
// running row by position). Persist, send AddTimer, then apply the normal start
// tail (confirmation + auto-return).
static void save_as_new_and_start(int32_t secs) {
  if (s_count >= MAX_TIMERS) {
    return;   // List full: nothing to create. (Keep it simple — no new row.)
  }
  if (secs < 1) { secs = 1; }
  int idx = s_count;
  Timer *t = &s_timers[idx];
  memset(t, 0, sizeof(*t));
  t->name[0] = 0;
  t->duration = secs;
  t->remaining = secs;
  t->state = TS_IDLE;
  t->custom = true;
  tc_start(t, now_s());            // -> RUNNING, end_time = now + secs
  s_count++;
  persist_all(); rearm_wakeup(); ensure_ticking();
  send_add_timer(secs);
  reload_ui();
  select_timer_row(idx);
  if (s_auto_return) { show_start_confirmation(idx); }   // flash -> watchface
  else { window_stack_remove(s_detail_window, true); }   // back to the list
}

// ---- window ----
static void window_load(Window *w) {
  Layer *root = window_get_root_layer(w);
  GRect bounds = layer_get_bounds(root);
  s_menu = menu_layer_create(bounds);
  menu_layer_set_callbacks(s_menu, NULL, (MenuLayerCallbacks){
    .get_num_rows = ml_num_rows,
    .get_cell_height = ml_cell_height,
    .draw_row = ml_draw_row,
    .select_click = ml_select,
    .select_long_click = ml_select_long,
  });
  menu_layer_set_click_config_onto_window(s_menu, w);
  layer_add_child(root, menu_layer_get_layer(s_menu));
}
static void window_unload(Window *w) { menu_layer_destroy(s_menu); s_menu = NULL; }

static void init(void) {
  s_count = store_load(s_timers);
  s_sort = (SortMode)store_load_sort();
  s_auto_return = store_load_autoreturn();
  s_running_first = store_load_runningfirst();
#ifdef SCREENSHOT_FIXTURES
  if (s_count == 0) {
    s_count = 3;
    memset(s_timers, 0, sizeof(s_timers));
    strcpy(s_timers[0].name, "Egg"); s_timers[0].duration = 300; s_timers[0].state = TS_RUNNING; s_timers[0].end_time = time(NULL) + 184; s_timers[0].last_used = time(NULL);
    strcpy(s_timers[1].name, "Tea"); s_timers[1].duration = 120; s_timers[1].state = TS_PAUSED; s_timers[1].remaining = 75; s_timers[1].last_used = time(NULL) - 10;
    strcpy(s_timers[2].name, "Laundry"); s_timers[2].duration = 3600; s_timers[2].state = TS_DONE; s_timers[2].remaining = 0; s_timers[2].last_used = 0;
  }
#endif
  // If launched by a wakeup, the firing event was already consumed; sweep now.
  WakeupId wid; int32_t cookie;
  bool by_wakeup = wakeup_get_launch_event(&wid, &cookie);
  if (by_wakeup) { store_save_wakeup_id(-1); }
  int fired = sweep_expiries();
  if (fired) { persist_all(); }
  rearm_wakeup();

  app_message_register_inbox_received(inbox_received);
  app_message_register_outbox_sent(outbox_sent);
  app_message_register_outbox_failed(outbox_failed);
  app_message_open(app_message_inbox_size_maximum(), app_message_outbox_size_maximum());
  request_config();   // pull config from the phone (covers app-closed-at-Save case)

  s_window = window_create();
  window_set_window_handlers(s_window, (WindowHandlers){ .load = window_load, .unload = window_unload });
  window_stack_push(s_window, true);
  rebuild_order();
  ensure_ticking();

  // Launched by a wakeup because a timer finished -> show the alarm over the list.
  if (by_wakeup && fired) { trigger_alarm(s_last_fired_idx, fired); }
}

static void deinit(void) {
  if (s_tick) { app_timer_cancel(s_tick); }
  persist_all();
  rearm_wakeup();   // ensure the closed-app wakeup reflects final state
  if (s_confirm_window) { window_destroy(s_confirm_window); }
  window_destroy(s_window);
}

int main(void) { init(); app_event_loop(); deinit(); }
