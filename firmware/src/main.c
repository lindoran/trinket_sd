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

#include "wire.h"
#include "dispatch.h"
#include "subsys_testharness.h"
#include "subsys_spi.h"

#define KEY_ESC  0x1Bu

// Non-blocking check for an ESC keypress on the USB CDC console,
// toggling subsys_spi.c's routine debug printf()s (see
// subsys_spi_set_debug()'s header comment). Deliberately polled only
// here, once per main-loop iteration between dispatch cycles -- NOT
// from inside wire.c's wire_wait_init_high()/wait loops, which are
// the timing-critical path every byte handshake goes through and
// stay exactly as confirmed-on-hardware. Practical effect: this only
// takes the keypress between top-level dispatch cycles, so toggle it
// at the DOS prompt before/after the operation you're timing, not
// expecting it to interrupt one already in flight.
static void poll_debug_toggle(void) {
    static bool spi_debug = false;   // mirrors what subsys_spi.c was last told

    int c = getchar_timeout_us(0);
    if (c == KEY_ESC) {
        spi_debug = !spi_debug;
        subsys_spi_set_debug(spi_debug);
        printf("trinket firmware: SPI debug logging %s\n", spi_debug ? "ON" : "OFF");
    }
}

int main(void) {
    stdio_init_all();
    wire_setup_pins();
    subsys_spi_init();       // bring up SPI0 (GPIO0-3) + CS at the safe default rate/mode
    wire_setup_reset_irq();

    // Give USB CDC a moment to enumerate before the first printf, purely
    // so early messages aren't lost while a terminal is still attaching.
    sleep_ms(1500);
    printf("trinket firmware: ready (dispatcher, 2 subsystems: test harness, SPI)\n");
    printf("trinket firmware: SPI debug logging OFF by default -- press ESC on this console to toggle\n");

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
            continue;
        }

        poll_debug_toggle();

        if (!dispatch_run_one()) {
            continue;   // wire_reset_pending() fired; handled at top of loop
        }
    }

    return 0;
}
