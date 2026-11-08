/*
 * dss_cmd.c -- the DISPATCH_CMD_DSS wire handler, see dss_cmd.h.
 */

#include "dss_cmd.h"

#include <stdint.h>

#include "wire.h"
#include "debug.h"

static bool cfg_recv_next_byte(uint8_t *out) {
    if (!wire_wait_init_high()) {
        return false;
    }
    return wire_finish_write_handshake(out);
}

static bool cfg_send_next_byte(uint8_t byte) {
    uint8_t corrected = byte ^ 0x88u;          // Busy (status bit 7) is inverted by the PC's port; see subsys_sd.c
    uint8_t low_nibble  = corrected & 0x0Fu;
    uint8_t high_nibble = (corrected >> 4) & 0x0Fu;

    if (!wire_finish_read_cycle(low_nibble, high_nibble)) return false;
    if (!wire_wait_init_high()) return false;
    return wire_finish_read_cycle(low_nibble, high_nibble);
}

static bool cfg_send_subsequent_byte(uint8_t byte) {
    if (!wire_wait_init_high()) return false;
    return cfg_send_next_byte(byte);
}

static bool cfg_run_write_side(dss_config_t *cfg) {
    uint8_t op;

    if (!wire_finish_write_handshake(&op)) {
        return false;
    }
    if (op == DSS_OP_SET) {
        uint8_t a[4];
        for (int i = 0; i < 4; i++) {
            if (!cfg_recv_next_byte(&a[i])) {
                return false;
            }
        }
        dss_config_t c;
        c.depth     = (uint16_t)(a[0] | ((uint16_t)a[1] << 8));
        c.warmup_ms = (uint16_t)(a[2] | ((uint16_t)a[3] << 8));
        bool clamped = dss_config_clamp(&c);
        *cfg = c;
        debug_printf("DSS: SET depth=%u warmup=%u ms%s\n", (unsigned)cfg->depth,
                     (unsigned)cfg->warmup_ms, clamped ? " (clamped)" : "");
    } else if (op == DSS_OP_GET) {
        debug_printf("DSS: GET\n");
    } else {
        debug_printf("DSS: unknown op 0x%02X -- ignoring\n", op);
    }
    return true;
}

static bool cfg_run_read_side(const dss_config_t *cfg) {
    uint8_t pkt[7];

    pkt[0] = 0;
    pkt[1] = (uint8_t)(cfg->depth & 0xFFu);
    pkt[2] = (uint8_t)(cfg->depth >> 8);
    pkt[3] = (uint8_t)(cfg->warmup_ms & 0xFFu);
    pkt[4] = (uint8_t)(cfg->warmup_ms >> 8);
    pkt[5] = (uint8_t)(DSS_FIFO_MAX & 0xFFu);
    pkt[6] = (uint8_t)(DSS_FIFO_MAX >> 8);

    if (!cfg_send_next_byte(pkt[0])) {
        return false;
    }
    for (int i = 1; i < 7; i++) {
        if (!cfg_send_subsequent_byte(pkt[i])) {
            return false;
        }
    }
    return true;
}

bool dss_cmd_run(dss_config_t *cfg) {
    if (!wire_wait_init_high()) {
        return false;
    }
    if (wire_is_write_selected()) {
        return cfg_run_write_side(cfg);
    }
    return cfg_run_read_side(cfg);
}
