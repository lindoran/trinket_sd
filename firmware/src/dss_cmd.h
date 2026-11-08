/*
 * dss_cmd.h -- the DISPATCH_CMD_DSS wire handler (FIFO depth configuration).
 *
 * Split out of subsys_dss.c so the byte-level protocol can be unit-tested on
 * a host against a mock wire layer (tests/test_dss_cmd.c): it depends only on
 * the wire.h handshake primitives, debug_printf and dss_model.h's config type.
 * See subsys_dss.h for the command's description.
 */

#ifndef TRINKET_DSS_CMD_H
#define TRINKET_DSS_CMD_H

#include <stdbool.h>
#include "dss_model.h"

#define DSS_OP_SET   0x01u
#define DSS_OP_GET   0x02u

/* Called by the dispatcher after the DISPATCH_CMD_DSS byte has been
 * consumed. Direction WRITE: an op byte (and for SET, four argument bytes)
 * follows, and *cfg is updated (clamped). Direction READ: sends the 7-byte
 * config packet. Returns false only if the wire gave up mid-transfer. */
bool dss_cmd_run(dss_config_t *cfg);

#endif /* TRINKET_DSS_CMD_H */
