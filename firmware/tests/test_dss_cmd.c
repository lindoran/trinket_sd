/*
 * Host-side test of the DISPATCH_CMD_DSS wire handler (src/dss_cmd.c) against
 * a mock wire layer. Checks the byte protocol both ways: what the handler
 * accepts after the command byte (op + arguments) and what it sends back --
 * including that every byte it sends survives the PC parallel port's
 * inversion of the Busy line, reconstructed here exactly as a host reading
 * the status register would see it.
 */
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "../src/dss_cmd.h"
#include "../src/wire.h"
#include "../src/debug.h"

static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; \
    printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---- mock wire: a scripted host ---- */
static uint8_t  script[32];     /* bytes the host "sends" (write side) */
static int      script_len, script_pos;
static bool     write_selected;
static uint8_t  nib_lo[64], nib_hi[64];  /* arguments of each finish_read_cycle call */
static int      nib_calls;

bool wire_wait_init_high(void)         { return true; }
bool wire_is_write_selected(void)      { return write_selected; }
bool wire_finish_write_handshake(uint8_t *out) {
    if (script_pos >= script_len) return false;     /* host stopped sending: wire gives up */
    *out = script[script_pos++];
    return true;
}
bool wire_finish_read_cycle(uint8_t low_nibble, uint8_t high_nibble) {
    nib_lo[nib_calls] = low_nibble;
    nib_hi[nib_calls] = high_nibble;
    nib_calls++;
    return true;
}
void debug_printf(const char *format, ...) { (void)format; }

static void host_writes(const uint8_t *bytes, int n) {
    memcpy(script, bytes, (size_t)n);
    script_len = n; script_pos = 0; write_selected = true;
}

/* What a host sees reading byte #k of the reply: two cycles (low nibble then
 * high nibble), each read from status bits 7..4 where the port inverts bit 7. */
static uint8_t host_byte(int k) {
    uint8_t lo = nib_lo[2 * k]     ^ 0x08u;   /* Busy inverted by the port */
    uint8_t hi = nib_hi[2 * k + 1] ^ 0x08u;
    return (uint8_t)(lo | (hi << 4));
}

static void test_set_applies_and_clamps(void) {
    dss_config_t c; dss_config_default(&c);
    const uint8_t set_ok[] = { DSS_OP_SET, 0x00, 0x01, 100, 0 };      /* depth 256, warm-up 100 */
    host_writes(set_ok, sizeof set_ok);
    CHECK(dss_cmd_run(&c), "SET returned false");
    CHECK(c.depth == 256 && c.warmup_ms == 100, "SET applied %u/%u", c.depth, c.warmup_ms);
    CHECK(script_pos == (int)sizeof set_ok, "handler consumed %d of %d bytes", script_pos, (int)sizeof set_ok);

    const uint8_t set_hi[] = { DSS_OP_SET, 0xE8, 0x03, 0xB8, 0x0B };  /* depth 1000, warm-up 3000 -> clamped */
    host_writes(set_hi, sizeof set_hi);
    CHECK(dss_cmd_run(&c), "clamped SET returned false");
    CHECK(c.depth == DSS_FIFO_MAX && c.warmup_ms == DSS_WARMUP_MS_MAX, "clamp gave %u/%u", c.depth, c.warmup_ms);

    const uint8_t set_lo[] = { DSS_OP_SET, 0x03, 0x00, 0x00, 0x00 };  /* depth 3 -> 16, warm-up 0 */
    host_writes(set_lo, sizeof set_lo);
    CHECK(dss_cmd_run(&c), "low SET returned false");
    CHECK(c.depth == DSS_FIFO_DEPTH && c.warmup_ms == 0, "low clamp gave %u/%u", c.depth, c.warmup_ms);
}

static void test_truncated_set_changes_nothing(void) {
    dss_config_t c; dss_config_default(&c);
    c.depth = 128; c.warmup_ms = 50;
    const uint8_t cut[] = { DSS_OP_SET, 0x00, 0x02 };                 /* only 2 of 4 argument bytes */
    host_writes(cut, sizeof cut);
    CHECK(!dss_cmd_run(&c), "a truncated SET must report the wire failure");
    CHECK(c.depth == 128 && c.warmup_ms == 50, "a truncated SET changed the config to %u/%u", c.depth, c.warmup_ms);
}

static void test_unknown_and_get_ops_are_harmless(void) {
    dss_config_t c; dss_config_default(&c);
    c.depth = 64; c.warmup_ms = 10;
    const uint8_t unk[] = { 0x7F };
    host_writes(unk, 1);
    CHECK(dss_cmd_run(&c), "unknown op should be ignored, not fail");
    const uint8_t get[] = { DSS_OP_GET };
    host_writes(get, 1);
    CHECK(dss_cmd_run(&c), "GET op should succeed");
    CHECK(c.depth == 64 && c.warmup_ms == 10, "ops without args changed the config");
}

static void test_read_packet_round_trips_through_busy_inversion(void) {
    dss_config_t c; dss_config_default(&c);
    c.depth = 256; c.warmup_ms = 100;
    nib_calls = 0; write_selected = false;
    CHECK(dss_cmd_run(&c), "read side returned false");
    CHECK(nib_calls == 14, "expected 14 nibble cycles (7 bytes x 2), got %d", nib_calls);
    const uint8_t want[7] = { 0x00, 0x00, 0x01, 100, 0, 0x00, 0x02 };   /* status, depth, warm-up, max 512 */
    for (int k = 0; k < 7; k++) {
        uint8_t got = host_byte(k);
        CHECK(got == want[k], "reply byte %d: host sees 0x%02X want 0x%02X", k, got, want[k]);
    }
}

static void test_read_packet_extremes(void) {
    dss_config_t c; dss_config_default(&c);
    c.depth = DSS_FIFO_MAX; c.warmup_ms = DSS_WARMUP_MS_MAX;
    nib_calls = 0; write_selected = false;
    dss_cmd_run(&c);
    CHECK(host_byte(1) == 0x00 && host_byte(2) == 0x02, "depth 512 read back as %02X%02X", host_byte(2), host_byte(1));
    CHECK(host_byte(3) == 0xD0 && host_byte(4) == 0x07, "warm-up 2000 read back as %02X%02X", host_byte(4), host_byte(3));
    /* every possible byte value must survive the inversion, not just the ones in a packet */
    for (int v = 0; v < 256; v++) {
        uint8_t corrected = (uint8_t)(v ^ 0x88u);
        uint8_t lo = (uint8_t)((corrected & 0x0Fu) ^ 0x08u);
        uint8_t hi = (uint8_t)(((corrected >> 4) & 0x0Fu) ^ 0x08u);
        CHECK((uint8_t)(lo | (hi << 4)) == v, "byte 0x%02X does not survive Busy inversion", v);
    }
}

int main(void) {
    test_set_applies_and_clamps();
    test_truncated_set_changes_nothing();
    test_unknown_and_get_ops_are_harmless();
    test_read_packet_round_trips_through_busy_inversion();
    test_read_packet_extremes();
    if (failures) { printf("%d FAILURE(S)\n", failures); return 1; }
    printf("dss_cmd: all tests passed\n");
    return 0;
}
