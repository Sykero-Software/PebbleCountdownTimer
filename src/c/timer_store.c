// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Tuomas Airaksinen
#include <pebble.h>
#include "timer_store.h"
#include <string.h>

static int load_list(int count_key, int base_key, Timer *out) {
  int count = persist_exists(count_key) ? persist_read_int(count_key) : 0;
  if (count > MAX_TIMERS) { count = MAX_TIMERS; }
  if (count < 0) { count = 0; }
  for (int i = 0; i < count; i++) {
    if (persist_exists(base_key + i)) {
      persist_read_data(base_key + i, &out[i], sizeof(Timer));
    } else {
      memset(&out[i], 0, sizeof(Timer));
    }
  }
  return count;
}

static void save_list(int count_key, int base_key, const Timer *t, int count) {
  if (count > MAX_TIMERS) { count = MAX_TIMERS; }
  persist_write_int(count_key, count);
  for (int i = 0; i < count; i++) {
    persist_write_data(base_key + i, &t[i], sizeof(Timer));
  }
  // drop any stale keys beyond the new count
  for (int i = count; i < MAX_TIMERS; i++) {
    if (persist_exists(base_key + i)) { persist_delete(base_key + i); }
  }
}

static int load_legacy(Timer *templates, Timer *instances) {
  int count = persist_exists(PERSIST_KEY_COUNT) ? persist_read_int(PERSIST_KEY_COUNT) : 0;
  if (count > MAX_TIMERS) { count = MAX_TIMERS; }
  if (count < 0) { count = 0; }
  int tc = 0;
  int ic = 0;
  for (int i = 0; i < count; i++) {
    Timer t;
    memset(&t, 0, sizeof(t));
    if (persist_exists(PERSIST_KEY_TIMER_BASE + i)) {
      persist_read_data(PERSIST_KEY_TIMER_BASE + i, &t, sizeof(Timer));
    }
    Timer tpl = t;
    tpl.state = TS_IDLE;
    tpl.end_time = 0;
    tpl.remaining = tpl.duration >= 1 ? tpl.duration : 1;
    tpl.custom = false;
    if (tc < MAX_TIMERS) { templates[tc++] = tpl; }
    if (t.state != TS_IDLE && ic < MAX_TIMERS) { instances[ic++] = t; }
  }
  return tc | (ic << 16);
}

static void ensure_schema(void) {
  persist_write_int(PERSIST_KEY_SCHEMA, STORE_SCHEMA);
}

static void migrate_legacy_if_needed(void) {
  if (!persist_exists(PERSIST_KEY_SCHEMA)) { return; }
  if (persist_read_int(PERSIST_KEY_SCHEMA) == STORE_SCHEMA) { return; }
  Timer templates[MAX_TIMERS];
  Timer instances[MAX_TIMERS];
  memset(templates, 0, sizeof(templates));
  memset(instances, 0, sizeof(instances));
  int packed = load_legacy(templates, instances);
  int tc = packed & 0xffff;
  int ic = (packed >> 16) & 0xffff;
  ensure_schema();
  save_list(PERSIST_KEY_TEMPLATE_COUNT, PERSIST_KEY_TEMPLATE_BASE, templates, tc);
  save_list(PERSIST_KEY_INSTANCE_COUNT, PERSIST_KEY_INSTANCE_BASE, instances, ic);
  if (persist_exists(PERSIST_KEY_COUNT)) { persist_delete(PERSIST_KEY_COUNT); }
  for (int i = 0; i < MAX_TIMERS; i++) {
    if (persist_exists(PERSIST_KEY_TIMER_BASE + i)) { persist_delete(PERSIST_KEY_TIMER_BASE + i); }
  }
}

int store_load_templates(Timer *out) {
  migrate_legacy_if_needed();
  if (!persist_exists(PERSIST_KEY_SCHEMA) ||
      persist_read_int(PERSIST_KEY_SCHEMA) != STORE_SCHEMA) { return 0; }
  return load_list(PERSIST_KEY_TEMPLATE_COUNT, PERSIST_KEY_TEMPLATE_BASE, out);
}

void store_save_templates(const Timer *t, int count) {
  ensure_schema();
  save_list(PERSIST_KEY_TEMPLATE_COUNT, PERSIST_KEY_TEMPLATE_BASE, t, count);
}

int store_load_instances(Timer *out) {
  migrate_legacy_if_needed();
  if (!persist_exists(PERSIST_KEY_SCHEMA) ||
      persist_read_int(PERSIST_KEY_SCHEMA) != STORE_SCHEMA) { return 0; }
  return load_list(PERSIST_KEY_INSTANCE_COUNT, PERSIST_KEY_INSTANCE_BASE, out);
}

void store_save_instances(const Timer *t, int count) {
  ensure_schema();
  save_list(PERSIST_KEY_INSTANCE_COUNT, PERSIST_KEY_INSTANCE_BASE, t, count);
}

int32_t store_load_wakeup_id(void) {
  if (!persist_exists(PERSIST_KEY_WAKEUPID)) { return -1; }
  return persist_read_int(PERSIST_KEY_WAKEUPID);
}

void store_save_wakeup_id(int32_t id) {
  persist_write_int(PERSIST_KEY_WAKEUPID, id);
}

int store_load_sort(void) {
  if (!persist_exists(PERSIST_KEY_SORTORDER)) { return SORT_MRU; }
  return persist_read_int(PERSIST_KEY_SORTORDER);
}

void store_save_sort(int mode) {
  persist_write_int(PERSIST_KEY_SORTORDER, mode);
}

bool store_load_autoreturn(void) {
  if (!persist_exists(PERSIST_KEY_AUTORETURN)) { return true; }   // default ON
  return persist_read_bool(PERSIST_KEY_AUTORETURN);
}

void store_save_autoreturn(bool on) {
  persist_write_bool(PERSIST_KEY_AUTORETURN, on);
}

bool store_load_runningfirst(void) {
  if (!persist_exists(PERSIST_KEY_RUNNINGFIRST)) { return true; }   // default ON
  return persist_read_bool(PERSIST_KEY_RUNNINGFIRST);
}

void store_save_runningfirst(bool on) {
  persist_write_bool(PERSIST_KEY_RUNNINGFIRST, on);
}

int store_load_idleexit(void) {
  if (!persist_exists(PERSIST_KEY_IDLEEXIT)) { return 15; }   // default 15s ON
  return persist_read_int(PERSIST_KEY_IDLEEXIT);
}

void store_save_idleexit(int seconds) {
  persist_write_int(PERSIST_KEY_IDLEEXIT, seconds);
}

bool store_load_launchsync(void) {
  if (!persist_exists(PERSIST_KEY_LAUNCHSYNC)) { return false; }   // default OFF
  return persist_read_bool(PERSIST_KEY_LAUNCHSYNC);
}

void store_save_launchsync(bool on) {
  persist_write_bool(PERSIST_KEY_LAUNCHSYNC, on);
}
