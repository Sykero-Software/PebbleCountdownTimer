// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Tuomas Airaksinen

import Clay from 'pebble-clay';
import clayConfig from './config_clay';
import timerListComponent from './config_timer_list';
import { timerListToString } from './timer_config';
import { resendDict } from './config_sync';

const clay = new Clay(clayConfig, null, { autoHandleEvents: false });
clay.registerComponent(timerListComponent);

// The watch requests its config on launch (any inbound AppMessage); reply with
// the last-saved values so config reaches the app even when it wasn't open at
// Save time. See config_sync.ts for why a watchapp needs this handshake.
Pebble.addEventListener('appmessage', () => {
  const dict = resendDict((k) => window.localStorage.getItem(k));
  if (!dict) { return; }
  Pebble.sendAppMessage(dict, () => { console.log('config resent'); },
    () => { console.log('config resend failed'); });
});

Pebble.addEventListener('showConfiguration', () => {
  Pebble.openURL(clay.generateUrl());
});

Pebble.addEventListener('webviewclosed', (e: any) => {
  if (!e || !e.response) { console.log('No settings changed'); return; }
  const raw = clay.getSettings(e.response, false);
  const s: Record<string, any> = {};
  Object.keys(raw).forEach((k) => {
    const v = raw[k];
    s[k] = (v && typeof v === 'object' && 'value' in v) ? v.value : v;
  });
  const dict: Record<string, any> = {};
  dict.TimerConfig = timerListToString(s.TimerList);
  dict.SortOrder = parseInt(s.SortOrder, 10) || 0;
  dict.AutoReturn = s.AutoReturn ? 1 : 0;
  dict.RunningFirst = s.RunningFirst ? 1 : 0;
  // persist so we can re-send when the watchapp later launches and asks (above)
  window.localStorage.setItem('timer_config', dict.TimerConfig);
  window.localStorage.setItem('sort_order', String(dict.SortOrder));
  window.localStorage.setItem('auto_return', String(dict.AutoReturn));
  window.localStorage.setItem('running_first', String(dict.RunningFirst));
  console.log('Sending TimerConfig: ' + JSON.stringify(dict.TimerConfig) + ' sort=' + dict.SortOrder + ' autoReturn=' + dict.AutoReturn);
  Pebble.sendAppMessage(dict, () => { console.log('config sent'); },
    () => { console.log('config send failed'); });
});
