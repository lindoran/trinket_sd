/*
 * dispatch.c -- see dispatch.h.
 */

#include "dispatch.h"

#include <stdio.h>
#include <stdint.h>

#include "wire.h"
#include "subsys_testharness.h"
#include "subsys_spi.h"
#include "subsys_sd.h"

bool dispatch_run_one(void) {
    // A command byte's own handshake looks exactly like any other
    // Init/Error write cycle at the wire level -- wait for Init, check
    // which direction the host selected, then run the handshake. The
    // only thing dispatch-specific here is that we require Write and
    // interpret the resulting byte as a command instead of payload.
    if (!wire_wait_init_high()) {
        return false;
    }

    if (!wire_is_write_selected()) {
        // Protocol misuse: a command byte only ever flows host->device,
        // so the host must select Write before raising Init for it. If
        // it selected Read instead, there's no command to dispatch and
        // nothing meaningful to send back -- we don't know what
        // subsystem the host wanted, so there's nothing to hand a Read
        // off to. Log it and let the host's own timeout catch this, the
        // same way any other malformed transfer gets caught on its
        // side (see driver/NOTES.md's ERR_NOT_READY path).
        printf("DISPATCH: expected command byte (Write), host selected Read -- ignoring\n");
        return true;
    }

    uint8_t cmd;
    if (!wire_finish_write_handshake(&cmd)) {
        return false;
    }

    switch (cmd) {
        case DISPATCH_CMD_TEST_HARNESS:
            return subsys_testharness_run();

        case DISPATCH_CMD_SPI:
            return subsys_spi_run();

        case DISPATCH_CMD_SD:
            return subsys_sd_run();

        default:
            // Unknown command: log and go back to idle. No payload
            // transfer happens for it, same as the Read-instead-of-
            // Write case above -- nothing implemented yet beyond the
            // one subsystem in dispatch.h's list.
            printf("DISPATCH: unknown command 0x%02X -- ignoring\n", cmd);
            return true;
    }
}
