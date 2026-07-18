// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Tuomas Airaksinen

// Clay config for the Countdown timer app. The single `timerList` custom
// component (messageKey 'TimerList') is a Clay-only key (NOT in package.json
// messageKeys) — index.ts serializes it to the real CString key 'TimerConfig'
// on save, exactly as TimeStyle maps WidgetList -> SettingWidgetList.
const config = [
  { type: 'heading', defaultValue: 'Countdown timer' },
  {
    type: 'section',
    items: [
      { type: 'heading', defaultValue: 'Timers' },
      { type: 'text', defaultValue: 'Add timers below. On the watch, open a timer to Start/Pause/Stop it.' },
      { type: 'timerList', messageKey: 'TimerList', defaultValue: [{ name: '', seconds: 0 }] },
    ],
  },
  {
    type: 'section',
    items: [
      { type: 'heading', defaultValue: 'Display' },
      // radiogroup values MUST be strings (Clay gotcha); index.ts parseInts on save.
      { type: 'radiogroup', messageKey: 'SortOrder', label: 'Sort timers on watch by',
        defaultValue: '0', options: [
          { label: 'Most recently used', value: '0' },
          { label: 'Shortest remaining first', value: '1' },
          { label: 'Longest remaining first', value: '2' },
        ] },
      { type: 'toggle', messageKey: 'RunningFirst',
        label: 'Show running timers at the top', defaultValue: true },
    ],
  },
  {
    type: 'section',
    items: [
      { type: 'heading', defaultValue: 'Behavior' },
      { type: 'toggle', messageKey: 'AutoReturn',
        label: 'Return to watchface after starting or stopping a timer',
        description: 'When on, the app closes back to the watchface once you start or stop a timer.',
        defaultValue: true },
      // select values MUST be strings (Clay); index.ts parseInts on save.
      { type: 'select', messageKey: 'IdleExitSec',
        label: 'Return to watchface when idle',
        description: 'Close back to the watchface after this many seconds with no button press in the timer list or detail view. Off disables it.',
        defaultValue: '15', options: [
          { label: 'Off', value: '0' },
          { label: '10 seconds', value: '10' },
          { label: '15 seconds', value: '15' },
          { label: '30 seconds', value: '30' },
          { label: '60 seconds', value: '60' },
        ] },
      { type: 'select', messageKey: 'SnoozeSec',
        label: 'Quick snooze (Up button)',
        description: 'When a timer finishes, the Up button snoozes it for this long. Off hides the Up snooze — you can still snooze from the Select menu.',
        defaultValue: '60', options: [
          { label: 'Off',    value: '0' },
          { label: '1 min',  value: '60' },
          { label: '3 min',  value: '180' },
          { label: '5 min',  value: '300' },
          { label: '10 min', value: '600' },
          { label: '15 min', value: '900' },
          { label: '30 min', value: '1800' },
          { label: '45 min', value: '2700' },
          { label: '60 min', value: '3600' },
        ] },
    ],
  },
  {
    type: 'section',
    items: [
      { type: 'heading', defaultValue: 'Using the timers on your watch' },
      { type: 'text', defaultValue:
        '<b>Timer list</b><br>' +
        '• <b>Up / Down</b> — move between timers.<br>' +
        '• <b>Select</b> (short press) — on a stopped timer, starts it right away; ' +
        'on a running or paused timer, opens its menu.<br>' +
        '• <b>Select (long press / hold)</b> — opens the menu for <i>any</i> timer. ' +
        'This is the only way to reach <b>+1 min / -1 min</b>, <b>Start &amp; Save</b> and ' +
        '<b>Delete</b> for a timer that has not been started yet.' },
      { type: 'text', defaultValue:
        '<b>Timer menu</b> (opened as above)<br>' +
        '• <b>Start / Pause / Stop</b> — control the timer.<br>' +
        '• <b>+1 min / -1 min</b> — adjust its time.<br>' +
        '• <b>Start &amp; Save</b> — after adjusting, start it and save the new time ' +
        'as a separate timer.<br>' +
        '• <b>Delete</b> — remove the timer (asks to confirm).' },
      { type: 'text', defaultValue:
        '<b>When a timer reaches zero</b><br>' +
        '• <b>Up</b> — quick snooze (the length set above). ' +
        '• <b>Select</b> — choose a snooze length from a menu. ' +
        '• <b>Down</b> — Stop. ' +
        '• <b>Back</b> — snooze (or Stop when quick snooze is Off).' },
    ],
  },
  { type: 'submit', defaultValue: 'Save' },
];

export = config;
