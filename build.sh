#!/usr/bin/env bash
# Single entry point for everything this repo can build: firmware +
# client tools (build-firmware.sh, which also builds firmware/client
# via ia16-elf-gcc) and the DOS driver (build-driver.sh, Borland C++
# 3.1 under dosbox). See TOOLCHAIN.md for what each of these actually
# needs installed, and why they're three genuinely different
# toolchains rather than one this script could unify further.
#
# Runs both regardless of whether one fails, so partial progress (e.g.
# firmware toolchain set up but not the driver's, or vice versa) still
# gets built -- and reports a summary at the end. Exits nonzero overall
# if either failed, so this is still safe to use as a CI-style gate.
set -u

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

firmware_status=0
driver_status=0

printf '=== firmware + client tools ===\n'
if "$ROOT_DIR/build-firmware.sh"; then
    firmware_status=0
else
    firmware_status=$?
fi

printf '\n===                 DOS driver                   ===\n'
printf '=== (Borland C++ 3.1 under dosbox dummy session) ===\n'

if "$ROOT_DIR/build-driver.sh"; then
    driver_status=0
else
    driver_status=$?
fi

printf '\n===                  summary                     ===\n'
if [ "$firmware_status" -eq 0 ]; then
    printf '\nfirmware + client tools: OK\n'
else
    printf '\nfirmware + client tools: FAILED (exit %d) -- see output above\n' "$firmware_status"
fi
if [ "$driver_status" -eq 0 ]; then
    printf 'DOS driver:              OK\n'
else
    printf 'DOS driver:              FAILED (exit %d) -- see output above\n' "$driver_status"
fi

if [ "$firmware_status" -ne 0 ] || [ "$driver_status" -ne 0 ]; then
    exit 1
fi
