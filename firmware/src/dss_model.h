/*
 * dss_model.h -- pure-software model of the Disney Sound Source's
 * 16-deep sample FIFO and fixed-rate drain clock.
 *
 * No Pico SDK, no GPIO, no globals: everything the model needs (the
 * current time) is passed in, so tests/test_dss_model.c compiles and
 * runs it on a normal host with plain gcc. subsys_dss.c wraps this
 * with the real hardware (PIO strobe capture, the nAck "FIFO full"
 * output, the DAC ring).
 *
 * What it models (Programmer's Guide to the Disney Sound Source):
 *   - 16-entry FIFO; a byte is accepted on each strobe.
 *   - The D/A side pulls one byte per tick of a fixed ~7kHz clock.
 *   - nAck (LPT pin 10) is HIGH while the FIFO is full.
 *   - Nothing in the guide suggests a byte written to a full FIFO is
 *     stored -- it is counted as an overflow and dropped here.
 *   - When a tick finds the FIFO empty the DAC simply keeps its last
 *     value ("hold"). The model reproduces that by emitting the held
 *     sample for that tick, so the downstream stream stays gap-free
 *     and the DAC's underrun counter doesn't count normal silence.
 */

#ifndef TRINKET_DSS_MODEL_H
#define TRINKET_DSS_MODEL_H

#include <stdint.h>
#include <stdbool.h>

/* The firmware build runs the strobe-handling code out of RAM (flash
 * execution can stall for microseconds on a cache miss, and a Sound Source
 * program re-checks nAck within a couple of microseconds). The host unit
 * tests compile the same source with -DDSS_HOST_TEST and no Pico SDK. */
#ifndef DSS_HOST_TEST
#include "pico.h"
#define DSS_RAMFUNC(name) __not_in_flash_func(name)
#else
#define DSS_RAMFUNC(name) name
#endif

/* The real unit's FIFO is 16 bytes: that is the default depth and the one
 * presence detection (write 32 bytes blind, expect "full") depends on. The
 * firmware can be told to present a deeper FIFO (see DSS.md, "FIFO depth")
 * so that a program's timer interrupt, when it does get to run between disk
 * transactions, can write further ahead; DSS_FIFO_MAX is the ceiling. */
#define DSS_FIFO_DEPTH        16u
#define DSS_FIFO_MAX          512u    /* power of two: the ring indexes with a mask */
#define DSS_WARMUP_MS_DEFAULT 250u
#define DSS_WARMUP_MS_MAX     2000u
#define DSS_POWER_DOWN_US     1000u      /* pin 17 low this long = power-down, not a strobe pulse */
#define DSS_FRESH_ABSENCE_US  1000000u   /* away this long = a new program / power-up */

/* nAck (the "FIFO full" status) is raised when the FIFO holds one byte
 * fewer than its capacity (`cap`, 16 by default). Deliberate: a program's
 * next status read can follow its last strobe by only a couple of
 * microseconds, and the flag has to cross the PIO, the session loop and the
 * output buffer first, so it is sometimes that much late. One slot of margin
 * turns a flag that is one strobe late into a byte that is accepted instead
 * of one that is dropped. Cost: one byte (143 us) less lead. Presence
 * detection still passes: 16 are accepted and the flag is up from 15. */
#define DSS_RATE_HZ      7000u    /* nominal; the real unit is 7kHz +/- 5% */
#define DSS_HOLD_INIT    0x80u    /* DAC midpoint before any sample arrives */

typedef struct {
    uint8_t  fifo[DSS_FIFO_MAX];
    uint16_t head;          /* index of the oldest byte */
    uint16_t level;         /* bytes currently queued, 0..cap */
    uint16_t cap;           /* current capacity, DSS_FIFO_DEPTH..DSS_FIFO_MAX */
    uint8_t  hold;          /* last value the DAC output */
    uint8_t  anchored;      /* timeline valid (see dss_model_resume) */
    uint32_t period_q16;    /* drain period in microseconds, Q16 fixed point */
    uint64_t next_q16;      /* time of the next drain tick, microseconds << 16 */

    /* statistics, never reset by the model itself except dss_model_init() */
    uint32_t st_accepted;   /* bytes accepted into the FIFO */
    uint32_t st_overflow;   /* bytes written while full (dropped) */
    uint32_t st_played;     /* ticks that consumed a real FIFO byte */
    uint32_t st_held;       /* ticks that found the FIFO empty (hold) */
    uint32_t st_resyncs;    /* times the timeline had to skip ahead */
    uint32_t st_gap_drained;/* FIFO bytes played out "late" across a session gap */
    uint32_t st_flushed;    /* bytes handed to the DAC early when a session ended */
    uint16_t st_max_level;  /* highest FIFO level seen */
} dss_model_t;

/* rate_hz == 0 selects DSS_RATE_HZ. Also zeroes statistics. */
void dss_model_init(dss_model_t *m, uint32_t rate_hz);

/* Re-anchors the drain clock to `now_us` WITHOUT back-filling the time
 * since the last advance. Call when a session (re)starts: whatever the
 * link was doing while we weren't watching it, the model must not play
 * out a burst of "held" samples for that gap and push the real audio
 * behind them (that would add the gap's length to the latency). */
void dss_model_resume(dss_model_t *m, uint64_t now_us);

/* Like dss_model_resume(), but first plays out whatever the real unit
 * would have played during the gap. A real Sound Source keeps draining
 * its FIFO at 7 kHz whether or not anything is talking to it, so after
 * a disk transaction that took the port for tens of milliseconds the
 * FIFO is empty (it needs 2.3 ms to empty) and a game's next burst
 * refills all 16 slots. If the model instead froze while the session
 * was away, it would still report "full" on re-entry and the game's
 * first burst would only squeeze in a byte or two.
 *
 * Pops min(FIFO level, ticks the gap spans) bytes into out[] in order
 * (at most out_max), updates the held value, and then re-anchors the
 * timeline exactly as dss_model_resume() does -- the empty-FIFO "hold"
 * ticks of the gap are still NOT back-filled, so the audio isn't pushed
 * back by the length of the gap. Returns the number of bytes written to
 * out[]. Returns 0 (and just anchors) if the model was never anchored. */
uint32_t dss_model_resume_drain(dss_model_t *m, uint64_t now_us,
                                uint8_t *out, uint32_t out_max);

/* One strobe: returns true if the byte was accepted, false if the FIFO
 * had no room at all (16 bytes) and the byte was dropped (st_overflow
 * incremented). The nAck flag goes up one byte earlier (see above). */
bool dss_model_write(dss_model_t *m, uint8_t b);

/* The nAck flag (see above) -- NOT "no room left": a byte written while the
 * flag is up but the FIFO still has its spare slot is accepted. */
static inline bool dss_model_full(const dss_model_t *m) {
    return (uint32_t)m->level + 1u >= (uint32_t)m->cap;
}

/* Low 32 bits of the time (microseconds) the next drain tick is due, for a
 * cheap "is a tick due yet?" test in the strobe loop: with the current time
 * as a 32-bit microsecond count, (int32_t)(now32 - due32) >= 0. Valid while
 * the model is serviced at least once per tick (it is, every pass), so the
 * two are never more than ~143 us apart and the wrap-around is harmless. */
static inline uint32_t dss_model_next_due_us32(const dss_model_t *m) {
    return (uint32_t)(m->next_q16 >> 16);
}

/* Runs every drain tick that has elapsed up to `now_us`, writing one
 * output sample per tick to out[] (the popped byte, or the held value
 * if the FIFO was empty). Returns the number written, at most out_max.
 * If more ticks were due than fit, the leftover lateness is discarded
 * and st_resyncs is incremented rather than carried forward. */
uint32_t dss_model_advance(dss_model_t *m, uint64_t now_us,
                           uint8_t *out, uint32_t out_max);

/* Sets the FIFO capacity (clamped to DSS_FIFO_DEPTH..DSS_FIFO_MAX). If the
 * FIFO currently holds more than the new capacity the newest bytes are
 * dropped. */
void dss_model_set_depth(dss_model_t *m, uint32_t depth);

/* Pops up to out_max queued bytes in order, with no regard for time: used
 * to hand a deep FIFO's contents to the DAC when a session ends, so they
 * keep playing while the dispatcher owns the port. Updates the held value.
 * Returns the number written. */
uint32_t dss_model_flush(dss_model_t *m, uint8_t *out, uint32_t out_max);

/* ----------------------------------------------------------------------
 * Configuration policy (pure, host-testable)
 * ---------------------------------------------------------------------- */
typedef struct {
    uint16_t depth;       /* DSS_FIFO_DEPTH (the default) .. DSS_FIFO_MAX */
    uint16_t warmup_ms;   /* shallow-FIFO window after a power-up, 0..DSS_WARMUP_MS_MAX */
} dss_config_t;

void dss_config_default(dss_config_t *c);

/* Forces both fields into range. Returns true if anything had to change. */
bool dss_config_clamp(dss_config_t *c);

/* Depth to present when a session starts. A program that has been away for
 * DSS_FRESH_ABSENCE_US or more (or never seen) is treated as having just
 * powered the unit up and gets the real 16-byte FIFO for `warmup_ms`, so a
 * presence test that overruns the FIFO and expects "full" still passes;
 * *warming reports that. Otherwise (a short absence: the dispatcher took the
 * port for a disk transaction) the configured depth applies at once. */
uint16_t dss_entry_depth(const dss_config_t *c, bool had_previous_session,
                         uint32_t absence_us, bool *warming);

/* ----------------------------------------------------------------------
 * Power tracking (pin 17 level; pure, inline so the strobe loop can use it)
 * ---------------------------------------------------------------------- */
typedef struct {
    uint32_t low_since;   /* microsecond timestamp pin 17 was first seen low */
    bool     low;
} dss_power_t;

/* Feed it the pin-17 level on every pass. Returns true once, on the pass
 * where pin 17 goes high after having been low for DSS_POWER_DOWN_US or
 * more: a power-down followed by a power-up. A strobe pulse (a few
 * microseconds low) never qualifies. */
static inline bool dss_power_update(dss_power_t *p, uint32_t now_us, bool pin_high) {
    if (pin_high) {
        bool up = p->low && (uint32_t)(now_us - p->low_since) >= DSS_POWER_DOWN_US;
        p->low = false;
        return up;
    }
    if (!p->low) {
        p->low = true;
        p->low_since = now_us;
    }
    return false;
}

#endif /* TRINKET_DSS_MODEL_H */
