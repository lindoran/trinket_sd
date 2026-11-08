/*
 * Host-side test of dss_model.c. Build + run:  ./tests/run_tests.sh
 * No Pico SDK needed -- the model has no hardware dependencies.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/dss_model.h"

static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; \
    printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* us per tick at 7000 Hz = 142.857... */
static uint64_t ticks_to_us(double ticks) { return (uint64_t)(ticks * 1e6 / 7000.0 + 0.5); }

static void test_detection_overflow(void) {
    /* The guide-style presence test: shove 32 bytes in back to back and
     * expect the full flag. Exactly 16 are accepted, 16 dropped. */
    dss_model_t m; dss_model_init(&m, 0); dss_model_resume(&m, 1000);
    int accepted = 0;
    for (int i = 0; i < 32; i++) accepted += dss_model_write(&m, (uint8_t)i);
    CHECK(accepted == 16, "accepted=%d want 16", accepted);
    CHECK(dss_model_full(&m), "should be full");
    CHECK(m.st_overflow == 16, "overflow=%u want 16", m.st_overflow);
}

static void test_full_flag_flips_at_guard_level(void) {
    /* nAck goes up at 15, one short of the 16-byte default capacity. */
    dss_model_t m; dss_model_init(&m, 0); dss_model_resume(&m, 0);
    for (int i = 0; i < 14; i++) { dss_model_write(&m, 1); CHECK(!dss_model_full(&m), "flag up too early at %d", i + 1); }
    dss_model_write(&m, 1);
    CHECK(dss_model_full(&m), "flag not up at the guard level");
    CHECK(m.level == 15, "level=%u want 15", m.level);
}

static void test_one_slot_guard_absorbs_late_strobe(void) {
    /* The point of the guard: a strobe that lands after the flag should
     * have stopped the program (it was one strobe late) is ACCEPTED into
     * the spare slot, not dropped. Only a second late strobe is lost. */
    dss_model_t m; dss_model_init(&m, 0); dss_model_resume(&m, 0);
    for (int i = 0; i < 15; i++) dss_model_write(&m, 1);
    CHECK(dss_model_full(&m), "flag should be up");
    CHECK(dss_model_write(&m, 2), "late strobe #1 must be accepted into the spare slot");
    CHECK(m.st_overflow == 0, "overflow=%u after one late strobe", m.st_overflow);
    CHECK(!dss_model_write(&m, 3), "late strobe #2 has nowhere to go");
    CHECK(m.st_overflow == 1, "overflow=%u want 1", m.st_overflow);
}

static void test_next_due_us32(void) {
    dss_model_t m; dss_model_init(&m, 0); dss_model_resume(&m, 1000);
    uint8_t out[4];
    uint32_t due = dss_model_next_due_us32(&m);
    CHECK(due == 1142, "next due=%u want 1142 (1000 + 142.86 us)", due);
    CHECK(dss_model_advance(&m, 1141, out, sizeof out) == 0, "tick ran before it was due");
    CHECK(dss_model_advance(&m, 1143, out, sizeof out) == 1, "tick did not run when due");
    CHECK(dss_model_next_due_us32(&m) == 1285, "due after one tick=%u want 1285", dss_model_next_due_us32(&m));
    /* 32-bit wrap: the compare used by the firmware's hot loop */
    uint32_t now32 = 5u, due32 = 0xFFFFFFF0u;
    CHECK((int32_t)(now32 - due32) >= 0, "wrapped 'now' should read as past due");
    now32 = 0xFFFFFFF0u; due32 = 5u;
    CHECK((int32_t)(now32 - due32) < 0, "wrapped 'due' should read as not yet");
}

static void test_drain_rate(void) {
    /* Fill, then run exactly 1 second: ~7000 ticks, first 16 are real
     * bytes, rest are holds. */
    dss_model_t m; dss_model_init(&m, 0); dss_model_resume(&m, 0);
    for (int i = 0; i < 16; i++) dss_model_write(&m, (uint8_t)(100 + i));
    static uint8_t out[8000];
    uint32_t total = 0;
    for (uint64_t t = 0; t <= 1000000; t += 50) {      /* 50us service loop */
        total += dss_model_advance(&m, t, out + total, sizeof(out) - total);
    }
    CHECK(total >= 6998 && total <= 7002, "ticks in 1s = %u want ~7000", total);
    CHECK(m.st_played == 16, "played=%u want 16", m.st_played);
    CHECK(out[0] == 100 && out[15] == 115, "fifo order wrong: %u %u", out[0], out[15]);
    CHECK(out[16] == 115 && out[total - 1] == 115, "hold value wrong");
    CHECK(m.st_resyncs == 0, "unexpected resyncs=%u", m.st_resyncs);
}

static void test_fifo_empties_in_16_ticks(void) {
    dss_model_t m; dss_model_init(&m, 0); dss_model_resume(&m, 0);
    for (int i = 0; i < 16; i++) dss_model_write(&m, 7);
    uint8_t out[64];
    dss_model_advance(&m, ticks_to_us(1.0) + 2, out, sizeof out);
    CHECK(m.level == 15, "level=%u want 15", m.level);
    CHECK(dss_model_full(&m), "flag stays up at 15 (guard level)");
    dss_model_advance(&m, ticks_to_us(2.0) + 2, out, sizeof out);
    CHECK(!dss_model_full(&m), "full flag should clear once the level drops to 14");
    dss_model_advance(&m, ticks_to_us(16.0) + 2, out, sizeof out);
    CHECK(m.level == 0, "level=%u want 0 after 16 ticks", m.level);
}

static void test_hold_when_empty(void) {
    dss_model_t m; dss_model_init(&m, 0); dss_model_resume(&m, 0);
    uint8_t out[16];
    uint32_t n = dss_model_advance(&m, ticks_to_us(5.0) + 2, out, sizeof out);
    CHECK(n == 5, "n=%u want 5", n);
    for (uint32_t i = 0; i < n; i++) CHECK(out[i] == DSS_HOLD_INIT, "hold init wrong");
    CHECK(m.st_held == 5 && m.st_played == 0, "held=%u played=%u", m.st_held, m.st_played);
}

static void test_resume_does_not_backfill(void) {
    dss_model_t m; dss_model_init(&m, 0); dss_model_resume(&m, 0);
    uint8_t out[4096];
    dss_model_advance(&m, 1000, out, sizeof out);
    /* link goes away for 2 seconds, then a new session starts */
    dss_model_resume(&m, 2000000);
    uint32_t n = dss_model_advance(&m, 2000000 + 10, out, sizeof out);
    CHECK(n == 0, "resume back-filled %u samples", n);
    n = dss_model_advance(&m, 2000000 + 150, out, sizeof out);
    CHECK(n == 1, "n=%u want 1 one tick after resume", n);
}

static void test_lateness_is_capped(void) {
    dss_model_t m; dss_model_init(&m, 0); dss_model_resume(&m, 0);
    uint8_t out[8];
    uint32_t n = dss_model_advance(&m, 1000000, out, sizeof out);   /* 1s late, room for 8 */
    CHECK(n == 8, "n=%u want 8", n);
    CHECK(m.st_resyncs == 1, "resyncs=%u want 1", m.st_resyncs);
    n = dss_model_advance(&m, 1000000 + 10, out, sizeof out);
    CHECK(n == 0, "backlog leaked: n=%u", n);
}

static void test_write_during_drain_interleaves(void) {
    /* Game-like loop: write whenever not full, service every 20us. The
     * model should hold the long-run write rate at the drain rate. */
    dss_model_t m; dss_model_init(&m, 0); dss_model_resume(&m, 0);
    uint8_t out[16]; uint32_t written = 0;
    for (uint64_t t = 0; t < 2000000; t += 20) {
        dss_model_advance(&m, t, out, sizeof out);
        while (!dss_model_full(&m)) { dss_model_write(&m, 1); written++; }
    }
    double rate = written / 2.0;
    CHECK(rate > 6990.0 && rate < 7030.0, "paced write rate %.1f want ~7000 (+16 initial)", rate);
    CHECK(m.st_overflow == 0, "overflow=%u", m.st_overflow);
}

static void test_custom_rate(void) {
    dss_model_t m; dss_model_init(&m, 7350);   /* +5% edge of the real unit's tolerance */
    dss_model_resume(&m, 0);
    static uint8_t out[8000];
    uint32_t total = 0;
    for (uint64_t t = 0; t <= 1000000; t += 50) total += dss_model_advance(&m, t, out + total, sizeof(out) - total);
    CHECK(total >= 7348 && total <= 7352, "ticks=%u want ~7350", total);
}

static void test_gap_drain_plays_fifo_late(void) {
    /* Session was servicing the model, FIFO is full, session leaves for
     * a 34 ms disk chunk (two sectors), comes back. */
    dss_model_t m; dss_model_init(&m, 0); dss_model_resume(&m, 0);
    uint8_t out[32];
    for (int i = 0; i < 16; i++) dss_model_write(&m, (uint8_t)(10 + i));
    dss_model_advance(&m, 10, out, sizeof out);      /* last service just after start */
    CHECK(dss_model_full(&m), "should still be full");
    uint32_t n = dss_model_resume_drain(&m, 34000, out, sizeof out);
    CHECK(n == 16, "gap drain n=%u want 16", n);
    for (uint32_t i = 0; i < n; i++) CHECK(out[i] == 10 + i, "order wrong at %u: %u", i, out[i]);
    CHECK(m.level == 0, "level=%u want 0", m.level);
    CHECK(!dss_model_full(&m), "full flag must be clear after the gap");
    CHECK(m.hold == 25, "hold=%u want 25", m.hold);
    CHECK(m.st_gap_drained == 16, "gap_drained=%u", m.st_gap_drained);
    /* and the hold ticks of the gap are not back-filled */
    uint32_t k = dss_model_advance(&m, 34000 + 10, out, sizeof out);
    CHECK(k == 0, "backfilled %u samples after resume", k);
}

static void test_gap_drain_partial_gap(void) {
    /* 5.5 ticks away: exactly 5 bytes should have been played. */
    dss_model_t m; dss_model_init(&m, 0); dss_model_resume(&m, 0);
    uint8_t out[32];
    for (int i = 0; i < 16; i++) dss_model_write(&m, (uint8_t)i);
    uint32_t n = dss_model_resume_drain(&m, ticks_to_us(5.5), out, sizeof out);
    CHECK(n == 5, "n=%u want 5", n);
    CHECK(m.level == 11, "level=%u want 11", m.level);
}

static void test_gap_drain_short_and_edge_cases(void) {
    uint8_t out[32];
    dss_model_t m; dss_model_init(&m, 0); dss_model_resume(&m, 0);
    for (int i = 0; i < 4; i++) dss_model_write(&m, (uint8_t)i);
    CHECK(dss_model_resume_drain(&m, 50, out, sizeof out) == 0, "gap shorter than a tick must drain nothing");
    CHECK(m.level == 4, "level changed on a sub-tick gap");

    dss_model_t e; dss_model_init(&e, 0); dss_model_resume(&e, 0);
    CHECK(dss_model_resume_drain(&e, 1000000, out, sizeof out) == 0, "empty FIFO must drain nothing");
    CHECK(e.hold == DSS_HOLD_INIT, "hold changed with nothing played");

    dss_model_t u; dss_model_init(&u, 0);               /* never anchored */
    dss_model_write(&u, 9);
    CHECK(dss_model_resume_drain(&u, 1000000, out, sizeof out) == 0, "unanchored model must not drain");
    CHECK(u.anchored == 1, "should be anchored after resume_drain");

    dss_model_t c; dss_model_init(&c, 0); dss_model_resume(&c, 0);
    for (int i = 0; i < 16; i++) dss_model_write(&c, (uint8_t)i);
    CHECK(dss_model_resume_drain(&c, 1000000, out, 4) == 4, "out_max must cap the drain");
    CHECK(c.level == 12, "level=%u want 12 after capped drain", c.level);
}

static void test_feeder_refills_after_gap(void) {
    /* The real-world symptom: a game's feeder burst after a disk chunk.
     * Without the gap drain it could squeeze in ~2 bytes (FIFO still
     * "full"); with it, all 16 slots are free again. */
    uint8_t out[32];
    dss_model_t m; dss_model_init(&m, 0); dss_model_resume(&m, 0);
    for (int i = 0; i < 16; i++) dss_model_write(&m, 1);
    dss_model_advance(&m, 20, out, sizeof out);
    dss_model_resume_drain(&m, 34000, out, sizeof out);
    int accepted = 0;
    while (!dss_model_full(&m) && accepted < 64) { dss_model_write(&m, 2); accepted++; }
    CHECK(accepted == (int)(DSS_FIFO_DEPTH - 1u), "burst after the gap accepted %d, want %u", accepted, DSS_FIFO_DEPTH - 1u);

    /* Control: the old behaviour (plain resume) leaves it full. */
    dss_model_t o; dss_model_init(&o, 0); dss_model_resume(&o, 0);
    for (int i = 0; i < 16; i++) dss_model_write(&o, 1);
    dss_model_resume(&o, 34000);
    CHECK(dss_model_full(&o), "plain resume should keep the FIFO full (the bug being fixed)");
}

static void test_depth_configurable_and_clamped(void) {
    dss_model_t m; dss_model_init(&m, 0);
    CHECK(m.cap == 16, "default cap=%u", m.cap);
    dss_model_set_depth(&m, 4);
    CHECK(m.cap == 16, "depth below 16 must clamp up, got %u", m.cap);
    dss_model_set_depth(&m, 100000);
    CHECK(m.cap == DSS_FIFO_MAX, "depth above max must clamp, got %u", m.cap);
    dss_model_set_depth(&m, 64);
    dss_model_resume(&m, 0);
    for (int i = 0; i < 62; i++) { dss_model_write(&m, 1); CHECK(!dss_model_full(&m), "flag up too early at %d (cap 64)", i + 1); }
    dss_model_write(&m, 1);
    CHECK(m.level == 63 && dss_model_full(&m), "flag should rise at cap-1=63, level=%u", m.level);
    CHECK(dss_model_write(&m, 2), "late strobe must land in the spare slot");
    CHECK(!dss_model_write(&m, 3), "cap 64 should be full");
    CHECK(m.st_overflow == 1, "overflow=%u want 1", m.st_overflow);
}

static void test_shrinking_depth_drops_newest(void) {
    dss_model_t m; dss_model_init(&m, 0); dss_model_set_depth(&m, 32); dss_model_resume(&m, 0);
    for (int i = 0; i < 30; i++) dss_model_write(&m, (uint8_t)i);
    dss_model_set_depth(&m, 16);
    CHECK(m.level == 16, "level=%u want 16 after shrinking", m.level);
    uint8_t out[32];
    uint32_t n = dss_model_flush(&m, out, sizeof out);
    CHECK(n == 16 && out[0] == 0 && out[15] == 15, "oldest bytes must survive a shrink: n=%u first=%u last=%u", n, out[0], out[15]);
}

static void test_fifo_wraps_at_deep_capacity(void) {
    /* indexing is a mask over DSS_FIFO_MAX, independent of cap: push the head round the ring */
    dss_model_t m; dss_model_init(&m, 0); dss_model_set_depth(&m, 100); dss_model_resume(&m, 0);
    uint8_t out[600]; uint32_t seq = 0, expect = 0; int bad = 0;
    for (int round = 0; round < 40; round++) {
        for (int i = 0; i < 90; i++) dss_model_write(&m, (uint8_t)(seq++));
        uint32_t n = dss_model_flush(&m, out, sizeof out);
        for (uint32_t k = 0; k < n; k++) if (out[k] != (uint8_t)(expect++)) bad++;
    }
    CHECK(bad == 0, "%d bytes out of order after wrapping the ring", bad);
    CHECK(seq == expect, "lost bytes across wraps: wrote %u got %u", seq, expect);
}

static void test_flush_in_order_and_counts(void) {
    dss_model_t m; dss_model_init(&m, 0); dss_model_set_depth(&m, 40); dss_model_resume(&m, 0);
    for (int i = 0; i < 39; i++) dss_model_write(&m, (uint8_t)(100 + i));
    uint8_t out[64];
    uint32_t n = dss_model_flush(&m, out, 10);
    CHECK(n == 10 && out[0] == 100 && out[9] == 109, "partial flush wrong: n=%u", n);
    CHECK(m.level == 29, "level=%u want 29", m.level);
    n = dss_model_flush(&m, out, sizeof out);
    CHECK(n == 29 && out[0] == 110 && out[28] == 138, "rest of flush wrong: n=%u", n);
    CHECK(m.hold == 138, "hold=%u want 138", m.hold);
    CHECK(m.st_flushed == 39, "flushed=%u want 39", m.st_flushed);
    CHECK(dss_model_flush(&m, out, sizeof out) == 0, "empty flush must return 0");
}

static void test_config_clamp_and_defaults(void) {
    dss_config_t c; dss_config_default(&c);
    CHECK(c.depth == 16 && c.warmup_ms == 250, "defaults wrong: %u/%u", c.depth, c.warmup_ms);
    CHECK(!dss_config_clamp(&c), "defaults must be in range");
    c.depth = 3; c.warmup_ms = 60000;
    CHECK(dss_config_clamp(&c), "out-of-range must report a change");
    CHECK(c.depth == 16 && c.warmup_ms == DSS_WARMUP_MS_MAX, "clamped to %u/%u", c.depth, c.warmup_ms);
    c.depth = 9999; c.warmup_ms = 0;
    dss_config_clamp(&c);
    CHECK(c.depth == DSS_FIFO_MAX && c.warmup_ms == 0, "clamped to %u/%u", c.depth, c.warmup_ms);
}

static void test_entry_depth_policy(void) {
    dss_config_t c; dss_config_default(&c);
    bool warming;
    /* default depth: never deep, never warming */
    CHECK(dss_entry_depth(&c, true, 5000000, &warming) == 16 && !warming, "default config must stay 16");
    /* deep config, first ever session: detection-safe */
    c.depth = 256;
    CHECK(dss_entry_depth(&c, false, 0, &warming) == 16 && warming, "first session must warm up at 16");
    /* deep config, returning after a disk transaction (short absence): deep at once */
    CHECK(dss_entry_depth(&c, true, 30000, &warming) == 256 && !warming, "a disk-gap re-entry must be deep immediately");
    CHECK(dss_entry_depth(&c, true, DSS_FRESH_ABSENCE_US - 1, &warming) == 256 && !warming, "just under the fresh threshold is still a re-entry");
    /* away a long time: a new program */
    CHECK(dss_entry_depth(&c, true, DSS_FRESH_ABSENCE_US, &warming) == 16 && warming, "a long absence must warm up");
    /* warm-up disabled */
    c.warmup_ms = 0;
    CHECK(dss_entry_depth(&c, false, 0, &warming) == 256 && !warming, "warm-up 0 means deep from the start");
}

static void test_power_tracker(void) {
    dss_power_t p = { 0, false };
    /* a strobe pulse: a few microseconds low, then high */
    CHECK(!dss_power_update(&p, 1000, true), "idle high is not an event");
    CHECK(!dss_power_update(&p, 1010, false), "going low is not an event");
    CHECK(!dss_power_update(&p, 1016, true), "a 6 us strobe pulse must not look like a power-up");
    /* a real power-down: low for 50 ms, then high */
    CHECK(!dss_power_update(&p, 2000, false), "power-down start");
    CHECK(!dss_power_update(&p, 20000, false), "still down");
    CHECK(dss_power_update(&p, 52000, true), "high after a long low must report a power-up");
    CHECK(!dss_power_update(&p, 52010, true), "the event reports once");
    /* just under the threshold */
    CHECK(!dss_power_update(&p, 100000, false), "low again");
    CHECK(!dss_power_update(&p, 100000 + DSS_POWER_DOWN_US - 1, true), "a low just under the threshold is a pulse");
    /* 32-bit wrap of the microsecond clock */
    CHECK(!dss_power_update(&p, 0xFFFFFF00u, false), "low near the wrap");
    CHECK(dss_power_update(&p, 0xFFFFFF00u + 5000u, true), "wrapped time must still read as a long low");
}

int main(void) {
    test_detection_overflow();
    test_full_flag_flips_at_guard_level();
    test_one_slot_guard_absorbs_late_strobe();
    test_next_due_us32();
    test_drain_rate();
    test_fifo_empties_in_16_ticks();
    test_hold_when_empty();
    test_resume_does_not_backfill();
    test_lateness_is_capped();
    test_write_during_drain_interleaves();
    test_custom_rate();
    test_gap_drain_plays_fifo_late();
    test_gap_drain_partial_gap();
    test_gap_drain_short_and_edge_cases();
    test_feeder_refills_after_gap();
    test_depth_configurable_and_clamped();
    test_shrinking_depth_drops_newest();
    test_fifo_wraps_at_deep_capacity();
    test_flush_in_order_and_counts();
    test_config_clamp_and_defaults();
    test_entry_depth_policy();
    test_power_tracker();
    if (failures) { printf("%d FAILURE(S)\n", failures); return 1; }
    printf("dss_model: all tests passed\n");
    return 0;
}
