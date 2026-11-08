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

/* GPIO numbers other modules (subsys_dss.c's PIO capture) need. wire.c
 * is still the only file that drives the handshake pins; these exist so
 * there is exactly one place the numbers are written down. See the
 * README.md "GPIO Map". */
#define WIRE_GPIO_DATA_BASE   4u    /* D0-D7 = GPIO4-11, LPT pins 2-9 (in)   */
#define WIRE_GPIO_SELECT_IN   15u   /* LPT pin 17: direction latch / DSS strobe (in) */
#define WIRE_GPIO_NACK        19u   /* LPT pin 10: nibble line / DSS FIFO-full (out) */
#define WIRE_GPIO_NAUTOLF     13u   /* LPT pin 14: reset / half of the DSS signature (in) */
#define WIRE_GPIO_NINIT       14u   /* LPT pin 16: semaphore / other half of the signature (in) */

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
 *
 * Returns true when the reset completed. Returns false if, while
 * waiting for nAuto-LF to release, nInit went HIGH with nAuto-LF still
 * high: that is the Disney Sound Source signature (see
 * wire_dss_signature()), meaning this was never a reset at all -- a
 * game powered up the DSS. The pending flag is left set and nothing
 * has been cleared; the caller should start a DSS session instead of
 * resetting subsystems.
 */
bool wire_do_reset_sequence(void);

/* Disney Sound Source signature: nAuto-LF (GPIO13, LPT pin 14) AND
 * nInit (GPIO14, LPT pin 16) both HIGH. A DSS-aware program writes
 * control values 04h / 0Ch (power-up / strobe, per the DSS Programmer's
 * Guide): bit 1 clear puts pin 14 HIGH, bit 2 set puts pin 16 HIGH.
 * The trinket's own dispatch protocol never drives that combination --
 * its idle state is 02h (pin 14 LOW), a reset is 00h (pin 16 LOW), and
 * a transaction only raises nInit with pin 14 LOW -- so this level
 * pair is an unambiguous "a game owns the port" marker. */
bool wire_dss_signature(void);

/* Raw level of the nAuto-LF/reset line (GPIO13). */
bool wire_reset_line_high(void);

/* Force the pending flag on/off. subsys_dss.c clears it when a session
 * starts (the power-up OUT that begins a session also edges pin 14, so
 * the reset IRQ always fires on entry) and sets it again if the host
 * leaves the session by asserting a real reset. */
void wire_request_reset(void);
void wire_clear_reset_pending(void);

/* Drive LPT pin 10 (nAck, GPIO19) directly -- the DSS "FIFO full"
 * status line (high = full). Only for use inside a DSS session; the
 * dispatch protocol owns this pin as nibble bit 2 the rest of the
 * time, and wire_park_outputs() puts it back to LOW. */
void wire_set_nack(bool high);

/* Called from the nAuto-LF rising-edge interrupt (priority 0, RAM-resident,
 * so it neither waits behind USB/DMA interrupts nor takes flash cache
 * misses) when nInit is ALSO already high -- the Disney Sound Source
 * signature -- so the capture hardware can be armed in microseconds
 * instead of waiting for the main loop to notice. The hook must be
 * __not_in_flash_func, short, and must not block. NULL = none. */
void wire_set_signature_hook(void (*fn)(void));

/* time_us_32() sampled inside that interrupt the last time nAuto-LF rose.
 * Used by subsys_dss.c to measure how long the main loop takes to start a
 * session after the edge. */
uint32_t wire_last_edge_us(void);

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
