/*
 * wire.h -- low-level RP2040 <-> LPT link primitives (doc secs 2, 5.1,
 * 5.2, nAuto-LF reset). Split out of the original single-file main.c
 * unchanged: same functions, same bodies, same call order, just moved
 * so dispatch.c and each subsystem (currently just subsys_testharness)
 * can share this instead of every subsystem reinventing byte-handshake
 * code. See README.md for how this project got here.
 *
 * Everything in this file was confirmed working on real hardware as
 * part of the original single-file main.c (device-side reset included).
 * The split itself -- these exact functions moved into their own
 * translation unit -- has not been separately re-run on hardware yet,
 * though it should be behaviorally identical.
 */

#ifndef TRINKET_WIRE_H
#define TRINKET_WIRE_H

#include <stdint.h>
#include <stdbool.h>

/* Call once at startup, before anything else in this file. */
void wire_setup_pins(void);

/* Call once, after wire_setup_pins() -- separate so the IRQ is armed
 * only once everything else is already initialized, no window where a
 * callback could fire against half-set-up state. */
void wire_setup_reset_irq(void);

/* True if the nAuto-LF reset line is currently asserted (or was since
 * the last wire_do_reset_sequence() call) and hasn't been serviced yet.
 * Callers (dispatch.c, subsystems) should check this at safe points and
 * unwind back to main()'s top-level loop when it's true, same as they
 * already unwind on a false return from wire_wait_init_high/low() or
 * wire_finish_write_handshake()/wire_finish_read_cycle() below -- all
 * signal the same thing. */
bool wire_reset_pending(void);

/* Runs the reset sequence: park outputs at a known-idle level, block
 * until the host releases nAuto-LF (line back low -- per README the
 * level-shifted line can be slow, so this is a plain polling wait, no
 * debounce logic beyond gpio_get() itself), then clear the pending
 * flag. Does NOT know about or clear any subsystem's own state (e.g.
 * subsys_testharness's last_byte) -- this file only owns the wire
 * itself. Callers are responsible for clearing subsystem state
 * afterward; see main.c.
 */
void wire_do_reset_sequence(void);

/* doc sec 5.3 -- current nSelect-In level. Callers sample this right
 * after a wire_wait_init_high() returns true, per the design note in
 * main.c's original header comment (sampling any earlier races against
 * the host). true = host selected Write, false = Read. */
bool wire_is_write_selected(void);

/* Block for the next Init rising/falling edge. Both return false if
 * aborted early because wire_reset_pending() went true during the
 * wait -- callers must check the return value and unwind back to their
 * caller (ultimately main()'s loop) rather than trusting nInit's state.
 * Bounded by WIRE_TIMEOUT_US: use these for any wait that happens once
 * a transaction is already in flight (a command byte or payload byte
 * has already been exchanged this dispatch cycle). */
bool wire_wait_init_high(void);
bool wire_wait_init_low(void);

/* Same as wire_wait_init_high() but never times out -- only returns
 * false on wire_reset_pending(). Use this ONLY for the top-level "no
 * command in flight yet" wait (dispatch_run_one()'s first call): the
 * host can sit idle between commands for an arbitrary amount of time,
 * so that wait must block indefinitely, not time out. */
bool wire_wait_for_command(void);

/* One full Init/Error byte-handshake cycle (doc sec 5.2), given Init is
 * already known to be high. *out is only valid if this returns true;
 * false means reset_pending fired mid-handshake. */
bool wire_finish_write_handshake(uint8_t *out);

/* One Init/Error nibble-handshake cycle (doc sec 5.1), given Init is
 * already known to be high for THIS cycle -- callers needing the full
 * two-cycle byte read call this twice, with a wire_wait_init_high() in
 * between (see subsys_testharness.c). Returns false if reset_pending
 * fired mid-cycle. */
bool wire_finish_read_cycle(uint8_t low_nibble, uint8_t high_nibble);

/* Dumps current physical pin states and wire status to stdout for diagnostic purposes. */
void wire_dump_diagnostics(void);

/* Parks all Pico-side LPT output lines (ERROR, nibble outputs) to their
 * known-idle level without blocking -- identical to what wire_do_reset_sequence()
 * does before it waits for nAuto-LF to release, but without requiring an actual
 * hardware reset pulse. Use this from the debug console ESC handler to bring the
 * output side of the link to a clean state after any diagnostic/print work. */
void wire_park_outputs(void);

#endif /* TRINKET_WIRE_H */
