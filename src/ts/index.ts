// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Tuomas Airaksinen

import Clay from 'pebble-clay';
import clayConfig from './config_clay';
import timerListComponent from './config_timer_list';
import { timerListToString } from './timer_config';

const clay = new Clay(clayConfig, null, { autoHandleEvents: false });
clay.registerComponent(timerListComponent);

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
  console.log('Sending TimerConfig: ' + JSON.stringify(dict.TimerConfig) + ' sort=' + dict.SortOrder);
  Pebble.sendAppMessage(dict, () => { console.log('config sent'); },
    () => { console.log('config send failed'); });
});
