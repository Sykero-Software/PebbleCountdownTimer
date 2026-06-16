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
      { type: 'toggle', messageKey: 'AutoReturn',
        label: 'Return to watchface after starting or stopping a timer', defaultValue: true },
      { type: 'toggle', messageKey: 'RunningFirst',
        label: 'Show running timers at the top', defaultValue: true },
    ],
  },
  { type: 'submit', defaultValue: 'Save' },
];

export = config;
