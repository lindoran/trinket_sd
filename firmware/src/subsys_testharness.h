/*
 * subsys_testharness.h -- the original trinket_ifacetest echo behavior
 * (see README.md), now living behind DISPATCH_CMD_TEST_HARNESS instead
 * of being unconditionally the only thing this firmware does. Unchanged
 * behavior from before dispatch.c existed: WRITE stores the byte as
 * last_byte; READ sends last_byte back (doc sec 7 correction + nibble
 * split). Still no queue, no framing beyond the dispatch byte itself,
 * no error detection beyond the handshakes' own semaphore timing.
 */

#ifndef TRINKET_SUBSYS_TESTHARNESS_H
#define TRINKET_SUBSYS_TESTHARNESS_H

#include <stdbool.h>

/* Runs exactly one transfer once dispatched to: a WRITE stores a byte,
 * a READ sends one back, host's choice via nSelect-In, sampled the same
 * way and for the same reason as the original main.c's top-level loop
 * (see this subsystem's .c file). Returns false if wire_reset_pending()
 * fired mid-transfer -- caller (dispatch.c) propagates that up
 * unchanged. */
bool subsys_testharness_run(void);

/* Clears this subsystem's own state (last_byte). wire.c's reset
 * sequence doesn't know this subsystem exists, so it can't clear state
 * it doesn't own -- main.c calls this explicitly right after
 * wire_do_reset_sequence() returns. */
void subsys_testharness_reset(void);

#endif /* TRINKET_SUBSYS_TESTHARNESS_H */
