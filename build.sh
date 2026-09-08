#!/usr/bin/env bash
# Single entry point for everything this repo can build: RP2040
# firmware (build-firmware.sh, arm-none-eabi-gcc/Pico SDK) and the two
# DOS-side pieces -- the driver (build-driver.sh) and the diagnostic
# clients (build-client.sh) -- both Borland C++ 3.1 under dosbox as of
# 2026-09-08 (see TOOLCHAIN.md for why: ia16-elf-gcc, the client tools'
# previous toolchain, has no viable fresh-sandbox install path, while
# BC31 is already vendored and proven end-to-end -- this project now
# has two genuinely different toolchains, not three).
#
# Runs all three regardless of whether one fails, so partial progress
# still gets built -- and reports a summary at the end. Exits nonzero
# overall if any failed, so this is still safe to use as a CI-style
# gate.
set -u

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

firmware_status=0
driver_status=0
client_status=0

printf '=== RP2040 firmware ===\n'
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

printf '\n===              DOS diagnostic clients          ===\n'
printf '=== (Borland C++ 3.1 under dosbox dummy session) ===\n'

if "$ROOT_DIR/build-client.sh"; then
    client_status=0
else
    client_status=$?
fi

printf '\n===                  summary                     ===\n'
if [ "$firmware_status" -eq 0 ]; then
    printf '\nRP2040 firmware:         OK\n'
else
    printf '\nRP2040 firmware:         FAILED (exit %d) -- see output above\n' "$firmware_status"
fi
if [ "$driver_status" -eq 0 ]; then
    printf 'DOS driver:              OK\n'
else
    printf 'DOS driver:              FAILED (exit %d) -- see output above\n' "$driver_status"
fi
if [ "$client_status" -eq 0 ]; then
    printf 'DOS diagnostic clients:  OK\n'
else
    printf 'DOS diagnostic clients:  FAILED (exit %d) -- see output above\n' "$client_status"
fi

if [ "$firmware_status" -ne 0 ] || [ "$driver_status" -ne 0 ] || [ "$client_status" -ne 0 ]; then
    exit 1
fi
