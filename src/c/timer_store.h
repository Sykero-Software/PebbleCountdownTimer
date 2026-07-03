// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Tuomas Airaksinen
#pragma once
#include "timer_calc.h"
#include <stdint.h>

#define PERSIST_KEY_SCHEMA    1
#define PERSIST_KEY_COUNT     2
#define PERSIST_KEY_WAKEUPID  3
#define PERSIST_KEY_SORTORDER 4
#define PERSIST_KEY_AUTORETURN 5
#define PERSIST_KEY_RUNNINGFIRST 6
#define PERSIST_KEY_IDLEEXIT 7       // idle auto-exit timeout, seconds (0 = off)
#define PERSIST_KEY_LAUNCHSYNC 10    // launch-sync template starts (0/1)
#define PERSIST_KEY_TIMER_BASE 100   // timer i -> key 100+i (one Timer per key; 256B/key cap)
#define PERSIST_KEY_TEMPLATE_COUNT 8
#define PERSIST_KEY_INSTANCE_COUNT 9
#define PERSIST_KEY_TEMPLATE_BASE 200
#define PERSIST_KEY_INSTANCE_BASE 300
#define STORE_SCHEMA 3

// Loads template timers into out (capacity MAX_TIMERS); returns count.
int store_load_templates(Timer *out);
// Persists template timers.
void store_save_templates(const Timer *t, int count);
// Loads running/history instances into out (capacity MAX_TIMERS); returns count.
int store_load_instances(Timer *out);
// Persists instance timers.
void store_save_instances(const Timer *t, int count);
// Wakeup id (-1 when none).
int32_t store_load_wakeup_id(void);
void store_save_wakeup_id(int32_t id);
// Sort mode (defaults to SORT_MRU=0 when unset).
int store_load_sort(void);
void store_save_sort(int mode);
// Auto-return-to-watchface flag (defaults to false when unset).
bool store_load_autoreturn(void);
void store_save_autoreturn(bool on);
// Running-timers-first list ordering (defaults to true when unset).
bool store_load_runningfirst(void);
void store_save_runningfirst(bool on);
// Idle auto-exit timeout in seconds (defaults to 15 when unset; 0 = off).
int store_load_idleexit(void);
void store_save_idleexit(int seconds);
// Launch-sync template starts flag (defaults to false when unset).
bool store_load_launchsync(void);
void store_save_launchsync(bool on);
