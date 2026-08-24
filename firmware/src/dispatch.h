/*
 * dispatch.h -- command dispatcher for the trinket firmware.
 *
 * "Trinket" (wire.c/.h) stays a plain character-oriented comms layer:
 * the Init/Error byte and nibble handshakes are exactly what they were
 * before this file existed, unchanged. What's new is the MEANING of the
 * first byte of every transfer: it's now an 8-bit command that says
 * which subsystem handles whatever comes after it, instead of always
 * meaning "test payload."
 *
 * Protocol shape (not yet written up in docs/trinket_lpt_protocol.md --
 * that doc covers wire.c's layer, this is new on top of it):
 *
 *   1. Host sends one command byte via a normal Init/Error byte-write
 *      handshake (doc sec 5.2). This byte ALWAYS flows host->device,
 *      regardless of what happens next -- a command only ever selects a
 *      subsystem, it doesn't carry payload itself.
 *   2. dispatch_run_one() looks the byte up below and calls that
 *      subsystem's handler for exactly one transfer.
 *   3. The subsystem's handler owns everything from there: it decides
 *      what its own next transfer looks like (in trinket_ifacetest's
 *      one existing subsystem, that's still nSelect-In choosing Write
 *      vs Read, same as before dispatch existed).
 *   4. Control returns to dispatch_run_one()'s caller (main.c's loop),
 *      which calls it again for the next command byte.
 *
 * Every top-level transfer -- not just the first one in a "session" --
 * gets its own command byte. There's no persistent "current subsystem"
 * state to track between calls, which is deliberate: it keeps
 * dispatch_run_one() itself completely stateless, and it's what makes
 * driver/backend.c able to inject the command byte transparently on
 * every single backend_write_byte()/backend_read_byte() call without
 * either side needing to track a session. The obvious cost is one extra
 * byte-handshake round trip per transfer; not a concern yet with one
 * subsystem and no throughput requirement, worth reconsidering if a
 * future subsystem needs block transfers.
 */

#ifndef TRINKET_DISPATCH_H
#define TRINKET_DISPATCH_H

#include <stdbool.h>

/* Command codes. Deliberately just switched on directly in
 * dispatch_run_one() (dispatch.c), not a function-pointer table --
 * there's exactly one subsystem right now, and a table indexed by a
 * handful of commands buys nothing yet. Revisit if/when this grows
 * past a few cases.
 *
 * These are a DIFFERENT namespace from driver/device.h's CMD_* values.
 * Those are DOS's own device-driver request-header command codes
 * (CMD_INIT, CMD_ND_INPUT, ...) for routing INT 21h calls to
 * driver_dispatch() -- a completely different protocol, on a different
 * machine, in a different compiler/toolchain. The two happening to both
 * be "8-bit command dispatch" is a coincidence of shape, not a shared
 * contract; don't assume a numeric value means the same thing in both
 * places.
 */
#define DISPATCH_CMD_TEST_HARNESS  0x01u   /* see subsys_testharness.h */
#define DISPATCH_CMD_SPI           0x02u   /* see subsys_spi.h */

/* Waits for and handles exactly one dispatch cycle: block for the
 * command byte's Init pulse, read the byte, then hand off the very next
 * transfer to whichever subsystem it selects. Returns false if
 * wire_reset_pending() fired at any point -- during the command byte's
 * own handshake, or inside whatever subsystem it dispatched to -- same
 * abort-propagation convention as wire_wait_init_high/low() and
 * wire_finish_write_handshake()/wire_finish_read_cycle() -- same
 * signal, same rule for callers. Caller (main.c) must unwind to the
 * top of its own loop and let the reset sequence run rather than
 * calling this again immediately. */
bool dispatch_run_one(void);

#endif /* TRINKET_DISPATCH_H */
