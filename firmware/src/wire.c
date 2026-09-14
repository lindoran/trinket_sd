/*
 * wire.c -- see wire.h. Pin assignments, byte-handshake cycles, and the
 * nAuto-LF reset IRQ, unchanged from the original single-file main.c
 * that these were confirmed working on real hardware as part of.
 */

#include "wire.h"

#include <stdio.h>
#include "debug.h"
#include "pico/stdlib.h"
#include "hardware/gpio.h"

// ---------------------------------------------------------------------
// Pin assignments -- doc sec 2, identical to trinket_firmware/src/main.c
// ---------------------------------------------------------------------
#define PIN_DATA_BASE     4    // D0-D7, GPIO 4-11, LPT->Pico (input)
#define PIN_DATA_COUNT    8

#define PIN_STROBE       12    // nStrobe, LPT->Pico (input), read-dir nibble select
#define PIN_RESET        13    // nAuto-LF, LPT->Pico (input), host-driven reset
                                // (hw-inverted: idle=LOW, asserted=HIGH -- see
                                // wire_do_reset_sequence()'s comment below)
#define PIN_INIT         14    // nInit, LPT->Pico (input), host-driven semaphore
#define PIN_SELECT_IN    15    // nSelect-In, LPT->Pico (input), direction latch

#define PIN_NIBBLE_BASE  18    // Busy/Ack/PaperOut/Select, GPIO 18-21 (output)
#define PIN_NIBBLE_COUNT  4
#define PIN_ERROR        22    // nError, Pico->LPT (output), device-driven semaphore

// ---------------------------------------------------------------------
// Reset line (nAuto-LF / GPIO13) -- IRQ sets this flag, every blocking
// wait below checks it and bails out early. volatile: written from IRQ
// context, read from main-loop context.
// ---------------------------------------------------------------------
static volatile bool reset_pending = false;

// ---------------------------------------------------------------------
// Transport timeout -- wire_wait_init_high/low() used to be genuinely
// unbounded: if the host disappeared mid-handshake (client crashed,
// timed out on its own side and gave up, cable hiccup, etc.) with
// nothing asserting nAuto-LF, the Pico would sit in these loops
// forever. Since dispatch_run_one() and every subsystem's multi-byte
// handler drop back into one of these two waits between essentially
// every byte, that meant a single bad handshake could permanently
// strand the whole device -- no more dispatch, no ESC console
// handling, nothing, until a hardware reset pulse arrived (which nothing
// was left to send). See DAC_DESIGN_ITERATION_WORK.md for the
// diagnostic trail that found this.
//
// Fix: bound the wait. A timeout is handled exactly like reset_pending
// -- return false, let the caller unwind to dispatch_run_one() (or
// straight back to main()'s loop), which then just waits fresh for the
// next Init transition. No caller-side changes needed; every existing
// caller already treats a false return as "unwind, don't trust the
// pin state." timeout_count is exposed via wire_dump_diagnostics() so a
// real stuck-transaction can be told apart from a clean idle link.
// ---------------------------------------------------------------------
#define WIRE_TIMEOUT_US   250000u   // 250ms -- generous vs. any real handshake step, short enough to recover quickly

static volatile uint32_t timeout_count = 0;

static void reset_isr(uint gpio, uint32_t events) {
    (void)events;
    if (gpio == PIN_RESET) {
        reset_pending = true;
    }
}

bool wire_reset_pending(void) {
    return reset_pending;
}

// ---------------------------------------------------------------------
// Low-level pin helpers
// ---------------------------------------------------------------------

static uint8_t read_data_bus(void) {
    uint8_t v = 0;
    for (int i = 0; i < PIN_DATA_COUNT; i++) {
        if (gpio_get(PIN_DATA_BASE + i)) {
            v |= (uint8_t)(1u << i);
        }
    }
    return v;
}

// Nibble bit N (N=3 is the nibble's MSB) goes out on GPIO (18+3-N), i.e.
// GPIO18 carries nibble bit3 ... GPIO21 carries nibble bit0. This mirrors
// trinket_protocol.pio's "out pins,4" from an OSR loaded as nibble<<28
// (MSB-first onto ascending GPIO numbers) -- see that file's header
// comment and doc sec 9 "OSR shift direction" for the derivation.
static void write_nibble(uint8_t nibble) {
    gpio_put(PIN_NIBBLE_BASE + 0, (nibble >> 3) & 1);
    gpio_put(PIN_NIBBLE_BASE + 1, (nibble >> 2) & 1);
    gpio_put(PIN_NIBBLE_BASE + 2, (nibble >> 1) & 1);
    gpio_put(PIN_NIBBLE_BASE + 3, (nibble >> 0) & 1);
}

bool wire_is_write_selected(void) {
    return gpio_get(PIN_SELECT_IN) != 0;
}

// Both return false if aborted early because reset_pending went true, OR
// because the host never showed up within WIRE_TIMEOUT_US -- callers
// must check the return value and unwind back to the main loop rather
// than trusting nInit's state. A timeout also parks our output lines
// (ERROR + nibbles) immediately, same as the reset path, so a
// transaction we bailed out of mid-way never leaves ERROR asserted or a
// stale nibble on the bus for the next attempt to trip over.
bool wire_wait_init_high(void) {
    uint32_t start = time_us_32();
    while (!gpio_get(PIN_INIT)) {
        if (reset_pending) {
            return false;
        }
        if (time_us_32() - start > WIRE_TIMEOUT_US) {
            timeout_count++;
            debug_printf("WIRE: timeout waiting for Init HIGH\n");
            wire_park_outputs();
            return false;
        }
        tight_loop_contents();
    }
    return true;
}

// Same wait, no timeout -- for the one call site (dispatch_run_one()'s
// very first wait) where blocking indefinitely is correct, not a bug.
// That wait means "no command in flight yet, host hasn't started one" --
// the host can legitimately sit idle for seconds or minutes between
// dactest invocations, so timing it out was wrong: it made the device
// print a timeout and "recover" from a failure that was never
// happening, every 250ms, for the entire time nothing was going on.
// Every OTHER wire_wait_init_high() call happens after a transaction
// has already started (a command byte was already read, a multi-byte
// payload or response is already in flight) -- those genuinely can
// strand the device if the host disappears mid-handshake, so they keep
// the bounded version above.
bool wire_wait_for_command(void) {
    while (!gpio_get(PIN_INIT)) {
        if (reset_pending) {
            return false;
        }
        tight_loop_contents();
    }
    return true;
}

bool wire_wait_init_low(void) {
    uint32_t start = time_us_32();
    while (gpio_get(PIN_INIT)) {
        if (reset_pending) {
            return false;
        }
        if (time_us_32() - start > WIRE_TIMEOUT_US) {
            timeout_count++;
            debug_printf("WIRE: timeout waiting for Init LOW\n");
            wire_park_outputs();
            return false;
        }
        tight_loop_contents();
    }
    return true;
}

// ---------------------------------------------------------------------
// Setup
// ---------------------------------------------------------------------

void wire_setup_pins(void) {
    for (int i = 0; i < PIN_DATA_COUNT; i++) {
        gpio_init(PIN_DATA_BASE + i);
        gpio_set_dir(PIN_DATA_BASE + i, GPIO_IN);
    }

    gpio_init(PIN_STROBE);
    gpio_set_dir(PIN_STROBE, GPIO_IN);
    gpio_init(PIN_INIT);
    gpio_set_dir(PIN_INIT, GPIO_IN);
    gpio_init(PIN_SELECT_IN);
    gpio_set_dir(PIN_SELECT_IN, GPIO_IN);

    for (int i = 0; i < PIN_NIBBLE_COUNT; i++) {
        gpio_init(PIN_NIBBLE_BASE + i);
        gpio_set_dir(PIN_NIBBLE_BASE + i, GPIO_OUT);
        gpio_put(PIN_NIBBLE_BASE + i, 0);
    }

    gpio_init(PIN_ERROR);
    gpio_set_dir(PIN_ERROR, GPIO_OUT);
    gpio_put(PIN_ERROR, 0);
}

void wire_setup_reset_irq(void) {
    gpio_init(PIN_RESET);
    gpio_set_dir(PIN_RESET, GPIO_IN);
    gpio_set_irq_enabled_with_callback(PIN_RESET, GPIO_IRQ_EDGE_RISE, true,
                                        &reset_isr);
}

// ---------------------------------------------------------------------
// Handshakes -- doc sec 5.1 / 5.2, one cycle at a time, no PIO
// ---------------------------------------------------------------------

bool wire_finish_write_handshake(uint8_t *out) {
    uint8_t byte = read_data_bus();
    gpio_put(PIN_ERROR, 1);          // "got it"
    if (!wire_wait_init_low()) {     // host: "ok, clear your ready state"
        gpio_put(PIN_ERROR, 0);
        return false;
    }
    gpio_put(PIN_ERROR, 0);          // reset, ready for next
    *out = byte;
    return true;
}

bool wire_finish_read_cycle(uint8_t low_nibble, uint8_t high_nibble) {
    bool want_high = gpio_get(PIN_STROBE) != 0;   // raw GPIO level (sec 5.1 note)
    write_nibble(want_high ? high_nibble : low_nibble);
    gpio_put(PIN_ERROR, 1);                        // "ready"
    if (!wire_wait_init_low()) {                   // host: "got it"
        gpio_put(PIN_ERROR, 0);
        return false;
    }
    gpio_put(PIN_ERROR, 0);                         // reset
    return true;
}

// ---------------------------------------------------------------------
// Reset sequence -- detect reset -> wait for reset state to end ->
// caller (main.c) clears subsystem state -> resume normal operation.
// ---------------------------------------------------------------------

// Runs once reset_pending is seen true at a safe point (top of the main
// loop, or unwound there from a mid-handshake abort). Parks our outputs
// at their known-idle levels immediately -- we don't want to leave
// Error/nibble lines asserted from a handshake that just got cut off --
// then blocks until the host releases nAuto-LF (line back LOW), since
// per README the level-shifted line can be slow and we must not resume
// answering the host until it's actually back in its normal state.
//
// nAuto-LF (GPIO13, LPT pin 14, hw-inverted control bit 1 -- see
// README.md "GPIO Map" / "Host-side register inversion table") is
// wired as a host-driven reset line. Per that inversion table, the
// host's idle state (control bit1=1) puts the *physical* pin LOW, and
// asserting reset (control bit1=0) puts it HIGH -- so on the Pico side,
// "reset asserted" == GPIO13 reads HIGH, "reset released" == GPIO13
// reads LOW.
void wire_do_reset_sequence(void) {
    debug_printf("RESET: nAuto-LF asserted, entering known state\n");

    gpio_put(PIN_ERROR, 0);
    write_nibble(0);

    while (gpio_get(PIN_RESET)) {
        tight_loop_contents();
    }

    // Line is back to idle (LOW). Clear the flag last, right before
    // handing control back, so a reset that re-asserts the instant we
    // clear it is still caught (IRQ just sets it true again; nothing
    // here depends on precise timing).
    reset_pending = false;

    debug_printf("RESET: nAuto-LF released, resuming normal operation\n");
}

void wire_dump_diagnostics(void) {
    uint8_t data_bus = read_data_bus();
    bool strobe = gpio_get(PIN_STROBE) != 0;
    bool reset_pin = gpio_get(PIN_RESET) != 0;
    bool init = gpio_get(PIN_INIT) != 0;
    bool select_in = gpio_get(PIN_SELECT_IN) != 0;
    bool err = gpio_get(PIN_ERROR) != 0;

    printf("[LPT Parallel Link Status]\n");
    printf("  Data Bus (D0-D7, GPIO 4-11): 0x%02X\n", data_bus);
    printf("  nSTROBE    (GPIO 12): %u (Read nibble select: %s)\n", strobe ? 1 : 0, strobe ? "HIGH" : "LOW");
    printf("  nRESET     (GPIO 13): %u (Asserted/Pending: %s)\n", reset_pin ? 1 : 0, reset_pending ? "YES" : "NO");
    printf("  nINIT      (GPIO 14): %u (Transfer semaphore)\n", init ? 1 : 0);
    printf("  nSELECT_IN (GPIO 15): %u (Direction: %s)\n", select_in ? 1 : 0, select_in ? "WRITE" : "READ");
    printf("  nERROR     (GPIO 22): %u (Device ready handshake)\n", err ? 1 : 0);
    printf("  Nibble Out (GPIO 18-21): BUSY=%u PE=%u SEL=%u ACK=%u\n",
           gpio_get(PIN_NIBBLE_BASE + 0), gpio_get(PIN_NIBBLE_BASE + 1),
           gpio_get(PIN_NIBBLE_BASE + 2), gpio_get(PIN_NIBBLE_BASE + 3));
    printf("  Transport timeouts (WIRE_TIMEOUT_US=%u): %lu\n",
           (unsigned)WIRE_TIMEOUT_US, (unsigned long)timeout_count);
}

void wire_park_outputs(void) {
    // Identical to the first two lines of wire_do_reset_sequence() -- park our
    // output lines immediately without waiting for any hardware signal. Called
    // from the ESC debug handler after diagnostic printing, so the LPT link is
    // back at a clean level before the next dispatch_run_one() waits for Init.
    gpio_put(PIN_ERROR, 0);
    write_nibble(0);
}
