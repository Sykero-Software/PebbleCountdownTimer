# PebbleCountdownTimer

**Countdown timer** — a simple multi-timer watchapp for Pebble (C + Pebble SDK),
from [Sykerö Software](https://github.com/Sykero-Software).

Configure a list of named countdown timers (up to 16), each with its own duration,
entirely from the phone (a Clay config page that opens in the Core Devices app).
On the watch you see the list and can **start / pause / reset** each timer via a
per-row action menu. Multiple timers run simultaneously; each vibrates when it
reaches zero even when the app is closed (a single Pebble Wakeup is kept armed for
the soonest-expiring timer). The list can be sorted by most-recently-used,
shortest remaining, or longest remaining.

Configuration is phone-side only — the watch never creates or edits timers, just
controls the configured ones.

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
