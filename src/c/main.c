// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Tuomas Airaksinen
#include <pebble.h>
#include "timer_calc.h"
#include "timer_store.h"

static Window *s_window;
static MenuLayer *s_menu;
static AppTimer *s_tick;

static Timer s_timers[MAX_TIMERS];
static int s_count = 0;
static int s_order[MAX_TIMERS];   // display order, rebuilt on reload per s_sort
static SortMode s_sort = SORT_MRU;

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

// Mark every expired RUNNING timer DONE; vibrate once if any fired. Returns true if any.
static bool sweep_expiries(bool vibrate) {
  bool any = false;
  int64_t now = now_s();
  for (int i = 0; i < s_count; i++) {
    if (tc_check_expiry(&s_timers[i], now)) { any = true; }
  }
  if (any && vibrate) { vibes_long_pulse(); }
  return any;
}

// ---- 1s foreground tick: refresh running rows + catch foreground expiries ----
static void tick_cb(void *ctx) {
  bool fired = sweep_expiries(true);
  bool running = false;
  for (int i = 0; i < s_count; i++) { if (s_timers[i].state == TS_RUNNING) { running = true; } }
  reload_ui();
  if (fired) { persist_all(); rearm_wakeup(); }
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

static void act_toggle(ActionMenu *am, const ActionMenuItem *item, void *ctx) {
  int idx = (int)(intptr_t)action_menu_get_context(am);
  Timer *t = &s_timers[idx];
  if (t->state == TS_RUNNING) { tc_pause(t, now_s()); }
  else { tc_start(t, now_s()); }
  persist_all(); rearm_wakeup(); ensure_ticking(); reload_ui();
}

static void act_reset(ActionMenu *am, const ActionMenuItem *item, void *ctx) {
  int idx = (int)(intptr_t)action_menu_get_context(am);
  tc_reset(&s_timers[idx], now_s());
  persist_all(); rearm_wakeup(); reload_ui();
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
  char sub[40]; snprintf(sub, sizeof(sub), "%s  %s", rem, state_label(t->state));
  menu_cell_basic_draw(gctx, cell, t->name[0] ? t->name : "(unnamed)", sub, NULL);
}

static void ml_select(MenuLayer *ml, MenuIndex *ci, void *ctx) {
  if (s_count == 0) { return; }
  open_action_menu(s_order[ci->row]);
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
    Timer parsed[MAX_TIMERS];
    int pn = tc_parse_config(cfg->value->cstring, parsed, MAX_TIMERS);
    Timer merged[MAX_TIMERS];
    int mn = tc_reconcile(s_timers, s_count, parsed, pn, merged);
    memcpy(s_timers, merged, sizeof(Timer) * (size_t)mn);
    s_count = mn;
    sweep_expiries(false);
    persist_all(); rearm_wakeup(); ensure_ticking();
  }
  reload_ui();
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
  bool fired = sweep_expiries(by_wakeup);   // vibrate only on wakeup launch
  if (fired) { persist_all(); }
  rearm_wakeup();

  app_message_register_inbox_received(inbox_received);
  app_message_open(app_message_inbox_size_maximum(), app_message_outbox_size_maximum());

  s_window = window_create();
  window_set_window_handlers(s_window, (WindowHandlers){ .load = window_load, .unload = window_unload });
  window_stack_push(s_window, true);
  rebuild_order();
  ensure_ticking();
}

static void deinit(void) {
  if (s_tick) { app_timer_cancel(s_tick); }
  persist_all();
  rearm_wakeup();   // ensure the closed-app wakeup reflects final state
  window_destroy(s_window);
}

int main(void) { init(); app_event_loop(); deinit(); }
