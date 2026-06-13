# Countdown timer — convenience targets.
#
# The sandbox needs the headless QEMU wrapper for any pebble command that may
# touch the emulator; we export it for all targets (harmless for build/clean).
PEBBLE_QEMU_PATH ?= $(HOME)/.local/bin/qemu-pebble-headless
export PEBBLE_QEMU_PATH

EMU ?= emery                 # emulator board for *-emu targets (emery = Pebble Time 2)
PBW := build/PebbleCountdownTimer.pbw

.PHONY: build install install-emu test clean logs logs-emu

## build: compile the watchapp (tsc src/ts -> src/pkjs, then bundle the .pbw)
build:
	pebble build

## install: install on the real watch via the CloudPebble relay
##   (one-time: `pebble login`; needs `gie net loose` for the relay hosts)
install: build
	pebble install --cloudpebble $(PBW)

## install-emu: install on the headless emulator
##   (boot it first: `cd .. && scripts/pebble-emu-boot.sh $(EMU)`)
install-emu: build
	pebble install --emulator $(EMU)

## test: run the phone-side (JS) tests and the watch-side (C) unit test
test:
	npm test
	gcc -I src/c tests/test_timer_calc.c src/c/timer_calc.c -o /tmp/cdt_timer_calc_test && /tmp/cdt_timer_calc_test

## clean: remove build artifacts (also clears cached message-key macros)
clean:
	pebble clean

## logs: stream logs from the real watch (CloudPebble)
logs:
	pebble logs --cloudpebble

## logs-emu: stream logs from the emulator
logs-emu:
	pebble logs --emulator $(EMU)
