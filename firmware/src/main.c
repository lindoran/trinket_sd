/*
 * trinket_ifacetest/src/main.c
 *
 * Dispatcher-based firmware for the RP2040 <-> LPT link described in
 * docs/trinket_lpt_protocol.md. This grew out of what started as a
 * deliberately dumb, single-file interface test (prove the raw wire
 * works, nothing else) -- that proven wire-level plumbing now lives in
 * wire.c/.h unchanged, and this file just wires it up to a command
 * dispatcher (dispatch.c/.h) sitting in front of it. Still a SEPARATE
 * project from trinket_firmware -- no PIO, no checksums -- but it's no
 * longer just an echo test: it's the first real cut of the "TSRs and
 * other drivers talk to trinket through a dispatcher" design, now with
 * two subsystems behind it: the original echo test harness
 * (subsys_testharness.c/.h) and a basic SPI master (subsys_spi.c/.h,
 * real SPI0 hardware on GPIO0-3 / H3, see its header comment for the
 * sub-protocol). Both are still just building blocks -- neither is
 * SD/FAT16-specific; that logic belongs to a future host-side TSR
 * driving the SPI subsystem, not to this firmware.
 *
 * Behavior:
 *   - Loops forever. Each iteration is one full dispatch cycle
 *     (dispatch_run_one(), see dispatch.h): wait for a command byte,
 *     hand off the next transfer to whichever subsystem it selects.
 *   - DISPATCH_CMD_TEST_HARNESS reproduces the original echo behavior
 *     exactly (see subsys_testharness.c): WRITE stores a byte, READ
 *     sends it back with the doc sec 7 correction applied, no queue, no
 *     framing beyond the dispatch byte itself.
 *   - DISPATCH_CMD_SPI (subsys_spi.c) runs a small request/response
 *     sub-protocol of its own inside a single dispatch cycle -- WRITE
 *     issues one SPI operation (configure rate/mode, move CS, or run a
 *     full-duplex transfer), READ fetches the most recently completed
 *     transfer's result. See subsys_spi.h for the exact framing.
 *   - nAuto-LF reset (wire.c) is unchanged and still fully orthogonal
 *     to dispatch: it can interrupt a transfer at any point (a command
 *     byte's own handshake, or a subsystem's payload handshake), and
 *     this loop checks wire_reset_pending() first every iteration, runs
 *     wire_do_reset_sequence(), then explicitly clears every
 *     subsystem's own state (subsys_testharness_reset() and
 *     subsys_spi_reset()) before resuming -- wire.c doesn't know what
 *     subsystems exist, so it can't clear state it doesn't own.
 *
 * All GPIO numbers live in wire.c, taken directly from
 * docs/trinket_lpt_protocol.md sec 2, same pinout as the real firmware,
 * so this can run against the exact same hardware -- only the firmware
 * on the Pico changes.
 *
 * NOT implemented on purpose (out of scope for this pass):
 *   - SD/FAT16-specific behavior of any kind (CMD0/CMD8/ACMD41
 *     sequencing, R1/R7 parsing, block CRCs, ...). subsys_spi.c is
 *     basic SPI only -- that logic is a future host-side TSR's job,
 *     built on top of DISPATCH_CMD_SPI, not this firmware's.
 *   - Any other subsystem beyond the test harness and SPI. Adding one
 *     means writing a new subsys_*.c/.h and one more dispatch.c case,
 *     not touching wire.c or the dispatch mechanism itself -- same
 *     shape subsys_spi.c/.h just followed to get added.
 *   - Any signaling back to the host for a malformed or unknown command
 *     byte (or unknown SPI opcode) beyond a USB-serial log line (see
 *     dispatch.c, subsys_spi.c) -- the host's own handshake timeout is
 *     what currently catches that on its side.
 *
 * Bring-up aid: prints every dispatch/transfer to the USB CDC console
 * (picocom / PuTTY / whatever), independent of whatever the DOS side
 * shows, so a mismatch between "what Pico thinks it saw" and "what DOS
 * thinks it got" narrows down which side of the link the bug is on.
 *
 * HARDWARE STATUS: wire.c's contents (byte/nibble handshakes, nAuto-LF
 * reset) were confirmed working on real hardware as the original
 * single-file main.c. The split into wire.c/dispatch.c/
 * subsys_testharness.c, and the dispatch layer itself, have NOT been
 * separately re-tested yet -- the wire-level behavior should be
 * identical (same functions, same bodies, same call order), but the
 * new command-byte framing this file adds in front of it is new code,
 * treat it as "should be right by inspection," not "confirmed," until
 * it's actually run against real DOS-side client changes (see
 * client/ifacetest.c and driver/backend.c). subsys_spi.c is newer still
 * and entirely unbuilt (no arm-none-eabi-gcc/pico-sdk available in the
 * environment it was written in) -- see its own header comment for what
 * "checked by inspection only" means for it specifically, and test it
 * with a logic analyzer on GPIO0-3 before pointing it at any real SPI
 * device, SD card included.
 */

#include <stdio.h>
#include "pico/stdlib.h"
#include "pico/stdio.h"
#include "hardware/clocks.h"

#include "debug.h"
#include "wire.h"
#include "activity.h"
#include "dispatch.h"
#include "subsys_testharness.h"
#include "subsys_spi.h"
#include "subsys_sd.h"
#include "subsys_dac.h"

#define KEY_ESC  0x1Bu

// Non-blocking check for an ESC keypress on the USB CDC console.
// On ESC:
//   1. Dumps full hardware diagnostics unconditionally (plain printf, always visible).
//   2. Toggles verbose debug logging.
//   3. Flushes the USB CDC buffer so the output appears immediately.
//   4. Parks LPT wire outputs to their known-idle level (ERROR=0, nibbles=0).
//   5. Resets all subsystems to a clean state, then triggers a short test beep.
//
// Deliberately only polled here, once per main-loop iteration between dispatch
// cycles -- NOT from inside wire.c's blocking wait loops (the timing-critical
// path). Practical effect: the keypress is seen between top-level dispatch
// cycles, which is fine for a diagnostic tool (not a real-time control path).
static void poll_debug_toggle(void) {
    static bool debug_on = false;

    int c = getchar_timeout_us(0);
    if (c != KEY_ESC) return;

    // Toggle verbose debug logging.
    debug_on = !debug_on;
    debug_set_enabled(debug_on);
    activity_set_debug(debug_on);
    subsys_spi_set_debug(debug_on);
    subsys_sd_set_debug(debug_on);
    subsys_dac_set_debug(debug_on);

    // Print full hardware diagnostics (always, regardless of debug_on).
    printf("\n============================================================\n");
    printf("              TRINKET HARDWARE DIAGNOSTICS                  \n");
    printf("============================================================\n");
    printf("  clk_sys = %lu MHz | uptime = %lu ms | debug logging: %s\n",
           (unsigned long)(clock_get_hz(clk_sys) / 1000000u),
           (unsigned long)to_ms_since_boot(get_absolute_time()),
           debug_on ? "ON" : "OFF");
    printf("------------------------------------------------------------\n");
    wire_dump_diagnostics();
    printf("\n");
    subsys_sd_dump_diagnostics();
    printf("\n");
    subsys_dac_dump_diagnostics();
    printf("============================================================\n");
    printf("  Resetting all subsystems to known idle state...\n");

    // Reset all subsystems to clean idle state so the next DOS command
    // starts fresh -- same as the nAuto-LF reset path in main(), but
    // without requiring an actual hardware reset signal from DOS.
    subsys_testharness_reset();
    subsys_spi_reset();
    subsys_sd_reset();
    subsys_dac_reset();  // clears ring buffer, cancels tone, restores silence-playing state

    // Park LPT wire output lines to idle levels immediately, so the
    // next dispatch_run_one() starts with ERROR=0 and nibbles=0.
    wire_park_outputs();

    printf("  Triggering 1000Hz 200ms test beep...\n");
    printf("============================================================\n\n");

    // Flush the USB CDC buffer NOW so everything above appears in the
    // terminal immediately, before we go back to blocking in dispatch_run_one().
    stdio_flush();

    // Fire test beep -- plays in background via DMA, does not block.
    subsys_dac_play_tone(1000, 200, DAC_SCLK_INTERNAL);
}

int main(void) {
    stdio_init_all();
    activity_init();
    wire_setup_pins();
    subsys_spi_init();       // bring up SPI0 (GPIO0-3) + CS at the safe default rate/mode
    subsys_sd_init();        // redundant hardware bring-up, same pins -- see subsys_sd.h
    subsys_dac_init();       // bring up DAC subsystem (PIO I2S + DMA + SCLK)
    wire_setup_reset_irq();

    // Startup hardware test beep (896 Hz, 200 ms) -- plays immediately on boot
    subsys_dac_play_tone(896, 200, DAC_SCLK_INTERNAL);

    // Give USB CDC a moment to enumerate before the first diagnostic, purely
    // so early messages aren't lost while a terminal is still attaching.
    sleep_ms(1500);
    printf("\n\n============================================================\n");
    printf(" trinket firmware: ready (dispatcher: harness, SPI, SD, DAC)\n");
    printf(" Startup test beep triggered on boot (896Hz, 200ms).\n");
    printf(" Press ESC in this console anytime for full hardware diag.\n");
    printf("============================================================\n\n");

    while (true) {
        // Checked first, every iteration: covers reset asserted between
        // dispatch cycles AND a dispatch cycle (or the subsystem it
        // called into) aborting early because wire_reset_pending() went
        // true mid-transfer -- either way we land back here with the
        // flag still set and run the same sequence before touching the
        // wire again.
        if (wire_reset_pending()) {
            wire_do_reset_sequence();
            subsys_testharness_reset();
            subsys_spi_reset();
            subsys_sd_reset();
            subsys_dac_reset();
            continue;
        }

        poll_debug_toggle();

        if (!dispatch_run_one()) {
            continue;   // wire_reset_pending() fired; handled at top of loop
        }
    }

    return 0;
}
