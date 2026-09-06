/*
 * subsys_testharness.c -- see subsys_testharness.h.
 */

#include "subsys_testharness.h"

#include "debug.h"
#include <stdint.h>

#include "wire.h"

static uint8_t last_byte = 0;

void subsys_testharness_reset(void) {
    last_byte = 0;   // fresh state, matching power-on default
}

bool subsys_testharness_run(void) {
    // Direction is NOT sampled speculatively before this wait -- that
    // raced against the host in the original main.c: if the host takes
    // any real time (e.g. printf'ing a status line) between selecting
    // direction and raising Init, sampling nSelect-In too early can
    // catch a stale value and permanently desync the two sides. Block
    // for the actual Init pulse FIRST, then sample nSelect-In right at
    // that instant -- the protocol guarantees the host always sets
    // nSelect-In before raising Init for a given transfer (doc sec
    // 5.3), so sampling here is the only point that's guaranteed
    // accurate. Same fix, same rationale, as the original single-file
    // main.c's header comment described it.
    if (!wire_wait_init_high()) {
        return false;
    }
    bool is_write = wire_is_write_selected();

    if (is_write) {
        uint8_t byte;
        if (!wire_finish_write_handshake(&byte)) {
            return false;
        }
        last_byte = byte;
        debug_printf("WRITE: received 0x%02X\n", last_byte);
    } else {
        uint8_t corrected = last_byte ^ 0x88;               // doc sec 7
        uint8_t low_nibble  = corrected & 0x0F;
        uint8_t high_nibble = (corrected >> 4) & 0x0F;

        // cycle 1, Init already high
        if (!wire_finish_read_cycle(low_nibble, high_nibble)) {
            return false;
        }
        if (!wire_wait_init_high()) {                       // cycle 2
            return false;
        }
        if (!wire_finish_read_cycle(low_nibble, high_nibble)) {
            return false;
        }
        debug_printf("READ:  sent     0x%02X\n", last_byte);
    }

    return true;
}
