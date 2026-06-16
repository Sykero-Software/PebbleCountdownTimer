# PebbleCountdownTimer

**Countdown timer** — a simple multi-timer watchapp for Pebble (C + Pebble SDK),
from [Sykerö Software](https://github.com/Sykero-Software).

Configure a list of named countdown timers (up to 16), each with its own duration,
from the phone (a Clay config page that opens in the Core Devices app). On the
watch you see the list and, per timer, can **start / pause / resume / stop**,
**adjust the duration** (±1 min), and **delete** it — via a per-row detail menu
(short SELECT starts an idle timer; long SELECT opens the detail menu in any
state). You can also **create a one-off timer directly on the watch** ("Save as
new & start" from a tuned idle row), no phone needed. Multiple timers run
simultaneously; each vibrates when it reaches zero even when the app is closed (a
single Pebble Wakeup is kept armed for the soonest-expiring timer). The list can be
sorted by most-recently-used, shortest remaining, or longest remaining.

The phone config stays the single source of truth for the saved list — naming and
reordering are phone-side — but timers created, adjusted or deleted on the watch
are synced back to it.

## Build

```bash
npm install
pebble build                 # compiles src/ts -> src/pkjs (tsc) then bundles
pebble install --emulator emery
```

Phone-side config (`src/ts/*.ts`) is TypeScript compiled to `src/pkjs/*.js` by
`tsc` (the generated JS is gitignored). Watch logic lives in `src/c/`, with the
pure, host-testable core in `timer_calc.c` (`gcc -I src/c tests/test_timer_calc.c
src/c/timer_calc.c -o /tmp/t && /tmp/t`). Run the phone-side tests with `npm test`.

## License

GPL-3.0-only.

## Support

Questions, feedback or bug reports: <pebble.trackworktime@sykero.fi>
