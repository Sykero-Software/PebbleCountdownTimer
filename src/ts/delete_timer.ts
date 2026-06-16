// SPDX-License-Identifier: GPL-3.0-only

// Remove a watch-selected timer (by list index) from the phone's persisted config
// and the Clay config-page store, mirroring add_timer.ts. `get`/`set` read/write
// localStorage by key. Returns the new TimerConfig string, or null when `index`
// is out of range (storage left untouched in that case).
import { stringToTimerList, timerListToString } from './timer_config';

export function deleteTimer(
  get: (k: string) => string | null,
  set: (k: string, v: string) => void,
  index: any
): string | null {
  const i = Math.floor(Number(index));
  const list = stringToTimerList(get('timer_config') || '');
  if (!(i >= 0 && i < list.length)) { return null; }
  list.splice(i, 1);
  const str = timerListToString(list);
  set('timer_config', str);
  let cs: any = {};
  try { cs = JSON.parse(get('clay-settings') || '{}') || {}; } catch (e) { cs = {}; }
  cs.TimerList = list;
  set('clay-settings', JSON.stringify(cs));
  return str;
}
