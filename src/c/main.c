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

static void rebuild_order(void) { tc_display_order(s_timers, s_count, s_sort, now_s(), s_order); }

static void reload_ui(void) {
  rebuild_order();
  if (s_menu) { menu_layer_reload_data(s_menu); }
}

static void ensure_ticking(void);   // defined below; used by the alarm handlers

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
  // Stop: reset the finished timer directly from the alarm, then dismiss.
  if (s_alarm_idx >= 0 && s_alarm_idx < s_count) {
    tc_reset(&s_timers[s_alarm_idx], now_s());
    persist_all(); rearm_wakeup(); reload_ui();
  }
  window_stack_remove(s_alarm_window, true);
}

static void alarm_add_minute(ClickRecognizerRef rec, void *ctx) {
  // Snooze: run the finished timer for 1 more minute, then dismiss the alarm.
  if (s_alarm_idx >= 0 && s_alarm_idx < s_count) {
    tc_extend(&s_timers[s_alarm_idx], 60, now_s());
    persist_all(); rearm_wakeup(); ensure_ticking(); reload_ui();
  }
  window_stack_remove(s_alarm_window, true);
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
  if (fired) { persist_all(); rearm_wakeup(); trigger_alarm(s_last_fired_idx, fired); }
  s_tick = running ? app_timer_register(1000, tick_cb, NULL) : NULL;
}

static void ensure_ticking(void) {
  if (s_tick) { return; }
  for (int i = 0; i < s_count; i++) {
    if (s_timers[i].state == TS_RUNNING) { s_tick = app_timer_register(1000, tick_cb, NULL); return; }
  }
}

// ---- ActionMenu: Start/Pause + Reset for the selected timer ----
static ActionMenuLevel *s_action_root;

// After acting on a timer it re-sorts (e.g. floats to the top in MRU mode); move
// the menu cursor to follow it to its new row so the user needn't scroll to it.
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

static void act_toggle(ActionMenu *am, const ActionMenuItem *item, void *ctx) {
  int idx = (int)(intptr_t)action_menu_get_context(am);
  if (idx < 0 || idx >= s_count) { return; }
  Timer *t = &s_timers[idx];
  if (t->state == TS_RUNNING) { tc_pause(t, now_s()); }
  else { tc_start(t, now_s()); }
  persist_all(); rearm_wakeup(); ensure_ticking(); reload_ui();
  select_timer_row(idx);
}

static void act_reset(ActionMenu *am, const ActionMenuItem *item, void *ctx) {
  int idx = (int)(intptr_t)action_menu_get_context(am);
  if (idx < 0 || idx >= s_count) { return; }
  tc_reset(&s_timers[idx], now_s());
  persist_all(); rearm_wakeup(); reload_ui();
  select_timer_row(idx);
}

// Free the level hierarchy after the menu closes (else it leaks per open).
static void action_menu_did_close(ActionMenu *am, const ActionMenuItem *performed, void *ctx) {
  action_menu_hierarchy_destroy(action_menu_get_root_level(am), NULL, NULL);
  s_action_root = NULL;
}

static void open_action_menu(int timer_idx) {
  s_action_root = action_menu_level_create(2);
  Timer *t = &s_timers[timer_idx];
  action_menu_level_add_action(s_action_root,
    (t->state == TS_RUNNING) ? "Pause" : "Start", act_toggle, NULL);
  action_menu_level_add_action(s_action_root, "Reset", act_reset, NULL);
  ActionMenuConfig cfg = {
    .root_level = s_action_root,
    .context = (void *)(intptr_t)timer_idx,
    .colors = { .background = GColorChromeYellow, .foreground = GColorBlack },
    .did_close = action_menu_did_close,
    .align = ActionMenuAlignCenter,
  };
  action_menu_open(&cfg);
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
  char rem[16]; tc_format_remaining(rem, sizeof(rem), tc_remaining_now(t, now_s()));
  if (t->name[0]) {
    char sub[40]; snprintf(sub, sizeof(sub), "%s  %s", rem, state_label(t->state));
    menu_cell_basic_draw(gctx, cell, t->name, sub, NULL);
  } else {
    // No name configured: show the time as the title, state as the subtitle.
    menu_cell_basic_draw(gctx, cell, rem, state_label(t->state), NULL);
  }
}

static void ml_select(MenuLayer *ml, MenuIndex *ci, void *ctx) {
  if (s_count == 0) { return; }
  int idx = s_order[ci->row];
  // An unstarted (idle) timer has only one useful action — skip the menu, just start.
  if (s_timers[idx].state == TS_IDLE) {
    tc_start(&s_timers[idx], now_s());
    persist_all(); rearm_wakeup(); ensure_ticking(); reload_ui();
    select_timer_row(idx);
  } else {
    open_action_menu(idx);
  }
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
  });
  menu_layer_set_click_config_onto_window(s_menu, w);
  layer_add_child(root, menu_layer_get_layer(s_menu));
}
static void window_unload(Window *w) { menu_layer_destroy(s_menu); s_menu = NULL; }

static void init(void) {
  s_count = store_load(s_timers);
  s_sort = (SortMode)store_load_sort();
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
  window_destroy(s_window);
}

int main(void) { init(); app_event_loop(); deinit(); }
