// SPDX-License-Identifier: GPL-3.0-only
#include "timer_calc.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void) {
  Timer t[MAX_TIMERS];

  // --- tc_parse_config ---
  int n = tc_parse_config("Egg\x1f""300\x1e""Tea\x1f""120", t, MAX_TIMERS);
  assert(n == 2);
  assert(strcmp(t[0].name, "Egg") == 0 && t[0].duration == 300);
  assert(t[0].state == TS_IDLE && t[0].remaining == 300);
  assert(strcmp(t[1].name, "Tea") == 0 && t[1].duration == 120);
  assert(tc_parse_config("", t, MAX_TIMERS) == 0);
  // skip zero/invalid durations; cap at MAX_TIMERS
  assert(tc_parse_config("a\x1f""0\x1e""b\x1f""60", t, MAX_TIMERS) == 1);
  assert(strcmp(t[0].name, "b") == 0);

  // --- tc_format_remaining ---
  char b[16];
  tc_format_remaining(b, sizeof(b), 0);     assert(strcmp(b, "0:00") == 0);
  tc_format_remaining(b, sizeof(b), 65);    assert(strcmp(b, "1:05") == 0);
  tc_format_remaining(b, sizeof(b), 599);   assert(strcmp(b, "9:59") == 0);
  tc_format_remaining(b, sizeof(b), 3600);  assert(strcmp(b, "1:00:00") == 0);
  tc_format_remaining(b, sizeof(b), 3725);  assert(strcmp(b, "1:02:05") == 0);

  // --- transitions + remaining_now ---
  Timer x; memset(&x, 0, sizeof(x)); x.duration = 300; x.remaining = 300; x.state = TS_IDLE;
  tc_start(&x, 1000);
  assert(x.state == TS_RUNNING && x.end_time == 1300 && x.last_used == 1000);
  assert(tc_remaining_now(&x, 1000) == 300);
  assert(tc_remaining_now(&x, 1290) == 10);
  assert(tc_remaining_now(&x, 1400) == 0);   // clamps at 0
  tc_pause(&x, 1290);
  assert(x.state == TS_PAUSED && x.remaining == 10 && x.last_used == 1290);
  tc_start(&x, 2000);
  assert(x.end_time == 2010);                 // resumes from remaining
  tc_reset(&x, 2500);
  assert(x.state == TS_IDLE && x.remaining == 300 && x.last_used == 2500);

  // --- tc_extend: run for N more secs from now, regardless of prior state ---
  Timer ex; memset(&ex, 0, sizeof(ex)); ex.duration = 300; ex.state = TS_DONE; ex.remaining = 0;
  tc_extend(&ex, 60, 5000);
  assert(ex.state == TS_RUNNING && ex.end_time == 5060 && ex.last_used == 5000);

  // --- expiry ---
  Timer y; memset(&y, 0, sizeof(y)); y.duration = 60; y.state = TS_RUNNING; y.end_time = 100;
  assert(tc_check_expiry(&y, 50) == false);
  assert(tc_check_expiry(&y, 100) == true && y.state == TS_DONE && y.remaining == 0);
  assert(tc_check_expiry(&y, 200) == false); // already DONE, not re-fired

  // --- soonest_end ---
  Timer s[3]; memset(s, 0, sizeof(s));
  s[0].state = TS_RUNNING; s[0].end_time = 500;
  s[1].state = TS_PAUSED;  s[1].end_time = 0;
  s[2].state = TS_RUNNING; s[2].end_time = 300;
  int64_t soon;
  assert(tc_soonest_end(s, 3, &soon) == true && soon == 300);
  Timer none[1]; memset(none, 0, sizeof(none)); none[0].state = TS_IDLE;
  assert(tc_soonest_end(none, 1, &soon) == false);

  // --- display_order: SORT_MRU (most-recently-used first, ties by index) ---
  Timer d[3]; memset(d, 0, sizeof(d));
  d[0].last_used = 10; d[1].last_used = 0; d[2].last_used = 30;
  int order[3];
  tc_display_order(d, 3, SORT_MRU, 0, order);
  assert(order[0] == 2 && order[1] == 0 && order[2] == 1);

  // --- display_order: SORT_SHORTEST / SORT_LONGEST by remaining-at-now ---
  Timer e[3]; memset(e, 0, sizeof(e));
  e[0].state = TS_RUNNING; e[0].end_time = 1300;  // 300s left at now=1000
  e[1].state = TS_PAUSED;  e[1].remaining = 50;   // 50s
  e[2].state = TS_IDLE;    e[2].remaining = 120;  // 120s
  tc_display_order(e, 3, SORT_SHORTEST, 1000, order);
  assert(order[0] == 1 && order[1] == 2 && order[2] == 0);  // 50, 120, 300
  tc_display_order(e, 3, SORT_LONGEST, 1000, order);
  assert(order[0] == 0 && order[1] == 2 && order[2] == 1);  // 300, 120, 50

  // --- reconcile: unchanged row keeps RUNNING state; new row IDLE ---
  Timer cur[1]; memset(cur, 0, sizeof(cur));
  strcpy(cur[0].name, "Egg"); cur[0].duration = 300; cur[0].state = TS_RUNNING;
  cur[0].end_time = 9999; cur[0].last_used = 42;
  Timer cfg[2]; memset(cfg, 0, sizeof(cfg));
  strcpy(cfg[0].name, "Egg"); cfg[0].duration = 300; cfg[0].state = TS_IDLE; cfg[0].remaining = 300;
  strcpy(cfg[1].name, "New"); cfg[1].duration = 60; cfg[1].state = TS_IDLE; cfg[1].remaining = 60;
  Timer outr[MAX_TIMERS];
  int rn = tc_reconcile(cur, 1, cfg, 2, outr);
  assert(rn == 2);
  assert(outr[0].state == TS_RUNNING && outr[0].end_time == 9999 && outr[0].last_used == 42);
  assert(outr[1].state == TS_IDLE && outr[1].duration == 60 && strcmp(outr[1].name, "New") == 0);

  // --- tc_add: running extends end_time; paused grows remaining; stamps last_used ---
  Timer a; memset(&a, 0, sizeof(a)); a.duration = 300;
  tc_start(&a, 1000);                 // RUNNING, end_time = 1300
  tc_add(&a, 300, 1100);             // +5 min while running
  assert(a.state == TS_RUNNING && a.end_time == 1600 && a.last_used == 1100);
  assert(tc_remaining_now(&a, 1100) == 500);

  Timer p; memset(&p, 0, sizeof(p)); p.duration = 600; p.state = TS_PAUSED; p.remaining = 120;
  tc_add(&p, 60, 2000);             // +1 min while paused
  assert(p.state == TS_PAUSED && p.remaining == 180 && p.last_used == 2000);

  // idle / done: no-op (state and timing unchanged)
  Timer id; memset(&id, 0, sizeof(id)); id.duration = 90; id.state = TS_IDLE; id.remaining = 90;
  tc_add(&id, 600, 3000);
  assert(id.state == TS_IDLE && id.remaining == 90 && id.last_used == 0);
  Timer dn; memset(&dn, 0, sizeof(dn)); dn.state = TS_DONE; dn.remaining = 0;
  tc_add(&dn, 600, 3000);
  assert(dn.state == TS_DONE && dn.remaining == 0 && dn.last_used == 0);

  printf("All timer_calc tests passed\n");
  return 0;
}
