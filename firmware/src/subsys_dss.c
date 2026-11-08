/*
 * subsys_dss.c -- see subsys_dss.h.
 */

#include "subsys_dss.h"

#include <stdio.h>
#include <stdint.h>

#include "pico/stdlib.h"
#include "pico/stdio.h"
#include "hardware/pio.h"
#include "hardware/structs/timer.h"
#include "hardware/structs/sio.h"
#include "hardware/gpio.h"

#include "debug.h"
#include "wire.h"
#include "activity.h"
#include "dss_model.h"
#include "dss_cmd.h"
#include "subsys_dac.h"
#include "dss_capture.pio.h"

#define PIO_DSS            pio1
#define KEY_ESC            0x1Bu
#define CONSOLE_POLL_US    25000u     /* how often a session checks for ESC */
#define ADVANCE_BATCH      16u        /* max drain ticks pushed per loop pass */

_Static_assert(dss_capture_STROBE_GPIO == WIRE_GPIO_SELECT_IN,
               "dss_capture.pio's STROBE_GPIO must match wire.h's WIRE_GPIO_SELECT_IN");

static dss_model_t model;
static uint pio_sm_idx = 0;
static uint pio_prog_offset = 0;
static bool initialised = false;
static bool debug_enabled = false;

/* Entry bookkeeping. A session can be entered two ways: the nAuto-LF edge
 * interrupt arms the PIO capture itself (dss_edge_hook, microseconds after
 * the game's control write) and the main loop only picks the session up
 * later, or -- fallback, e.g. nInit hadn't risen yet when the edge fired --
 * the main loop arms it. Only the first protects the strobes that arrive in
 * the meantime. */
#define LAT_BUCKETS          6
#define PRE_ARM_MAX_AGE_US   3000u   /* an interrupt-armed capture older than this is stale */
static volatile bool     session_active = false;
static volatile bool     armed_by_irq = false;
static volatile uint32_t armed_t0 = 0;
static uint32_t entries_irq_armed = 0;
static uint32_t entries_main_armed = 0;
static uint32_t entries_fifo_full = 0;     /* PIO RX FIFO already full at entry: strobes were lost */
static uint32_t lat_hist[LAT_BUCKETS];     /* edge -> session start, microseconds */
static uint32_t lat_sum = 0, lat_n = 0, lat_max = 0;

/* FIFO depth configuration and the state that applies it (see dss_model.h).
 * The configured depth only takes effect once a program has had its
 * detection window: a program that has been away for DSS_FRESH_ABSENCE_US
 * (or a pin-17 power cycle) gets the real 16-byte FIFO for cfg.warmup_ms
 * first. */
#define FLUSH_RING_HEADROOM  256u   /* frames of DAC ring left free when flushing a deep FIFO */
static dss_config_t cfg = { DSS_FIFO_DEPTH, DSS_WARMUP_MS_DEFAULT };
static bool     warming = false;
static uint32_t warm_until = 0;
static dss_power_t power = { 0, false };
static uint32_t last_exit_us = 0;
static bool     have_exit = false;
static uint32_t warmups = 0;

static void lat_record(uint32_t us) {
    static const uint32_t limit[LAT_BUCKETS - 1] = { 10u, 25u, 50u, 100u, 250u };
    uint i = 0;
    while (i < LAT_BUCKETS - 1 && us >= limit[i]) i++;
    lat_hist[i]++;
    lat_sum += us;
    lat_n++;
    if (us > lat_max) lat_max = us;
}

static uint32_t session_count = 0;
static uint32_t last_session_us = 0;      /* duration of the most recent session */
static uint32_t total_captured = 0;       /* bytes taken from the PIO FIFO, all sessions */

/* Runs inside the nAuto-LF edge interrupt (wire.c) when nInit is also high.
 * RAM-resident, non-blocking: just restarts the capture state machine so the
 * very first strobe edge is seen, whatever the main loop happens to be doing
 * (USB, an SD transaction's tail, ...). The state machine's own 8-deep RX
 * FIFO holds what arrives until the session loop starts draining it. */
static void __not_in_flash_func(dss_edge_hook)(void) {
    if (!initialised || session_active) {
        return;
    }
    pio_sm_set_enabled(PIO_DSS, pio_sm_idx, false);
    pio_sm_clear_fifos(PIO_DSS, pio_sm_idx);
    pio_sm_restart(PIO_DSS, pio_sm_idx);
    pio_sm_exec(PIO_DSS, pio_sm_idx, pio_encode_jmp(pio_prog_offset));
    pio_sm_set_enabled(PIO_DSS, pio_sm_idx, true);
    armed_t0 = timer_hw->timerawl;
    armed_by_irq = true;
}

void subsys_dss_set_debug(bool enabled) {
    debug_enabled = enabled;
}

void subsys_dss_init(void) {
    dss_model_init(&model, DSS_RATE_HZ);
    dss_config_default(&cfg);

    pio_prog_offset = pio_add_program(PIO_DSS, &dss_capture_program);
    pio_sm_idx = (uint)pio_claim_unused_sm(PIO_DSS, true);

    pio_sm_config c = dss_capture_program_get_default_config(pio_prog_offset);
    sm_config_set_in_pins(&c, WIRE_GPIO_DATA_BASE);
    sm_config_set_in_shift(&c, false /* shift left: byte lands in the low 8 bits */,
                           true /* autopush */, 8);
    sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_RX);      /* 8-deep RX FIFO */
    sm_config_set_clkdiv(&c, 1.0f);                      /* full speed: ~3 cycles per capture */

    /* Same convention as the SDK's uart_rx example: hand the input pins to the
     * PIO block with their direction left as input. wire.c keeps reading them
     * with gpio_get(), which reflects the pad regardless of function select. */
    for (uint i = 0; i < 8; i++) {
        pio_gpio_init(PIO_DSS, WIRE_GPIO_DATA_BASE + i);
    }
    pio_gpio_init(PIO_DSS, WIRE_GPIO_SELECT_IN);
    pio_sm_set_consecutive_pindirs(PIO_DSS, pio_sm_idx, WIRE_GPIO_DATA_BASE, 8, false);
    pio_sm_set_consecutive_pindirs(PIO_DSS, pio_sm_idx, WIRE_GPIO_SELECT_IN, 1, false);

    pio_sm_init(PIO_DSS, pio_sm_idx, pio_prog_offset, &c);   /* leaves it disabled */
    initialised = true;
    wire_set_signature_hook(dss_edge_hook);
}

void subsys_dss_reset(void) {
    /* Clear the FIFO contents but keep the counters, which are the whole
     * point of looking at this after the fact. */
    dss_model_t fresh;
    dss_model_init(&fresh, DSS_RATE_HZ);
    fresh.st_accepted  = model.st_accepted;
    fresh.st_overflow  = model.st_overflow;
    fresh.st_played    = model.st_played;
    fresh.st_held      = model.st_held;
    fresh.st_resyncs   = model.st_resyncs;
    fresh.st_gap_drained = model.st_gap_drained;
    fresh.st_flushed   = model.st_flushed;
    fresh.st_max_level = model.st_max_level;
    model = fresh;
    warming = false;                       /* cfg (the depth setting) is deliberately kept */
}

void subsys_dss_dump_diagnostics(void) {
    printf("[Disney Sound Source emulation]\n");
    printf("  Signature now:   %s (nAuto-LF=%u nInit=%u)\n",
           wire_dss_signature() ? "PRESENT (game owns the port)" : "absent (dispatcher owns the port)",
           wire_reset_line_high() ? 1u : 0u, wire_dss_signature() ? 1u : 0u);
    printf("  Sessions:        %lu (last one %lu ms)\n",
           (unsigned long)session_count, (unsigned long)(last_session_us / 1000u));
    printf("  Strobes captured: %lu\n", (unsigned long)total_captured);
    printf("  FIFO model:     level=%u/%u max=%u accepted=%lu overflow(dropped)=%lu\n",
           (unsigned)model.level, (unsigned)DSS_FIFO_DEPTH, (unsigned)model.st_max_level,
           (unsigned long)model.st_accepted, (unsigned long)model.st_overflow);
    printf("  Drain ticks:    played=%lu held(empty)=%lu resyncs=%lu gap-drained=%lu\n",
           (unsigned long)model.st_played, (unsigned long)model.st_held,
           (unsigned long)model.st_resyncs, (unsigned long)model.st_gap_drained);
    printf("  FIFO depth:     configured=%u (default %u, max %u) warm-up=%u ms | now %u%s | warm-ups=%lu | flushed to DAC=%lu\n",
           (unsigned)cfg.depth, (unsigned)DSS_FIFO_DEPTH, (unsigned)DSS_FIFO_MAX, (unsigned)cfg.warmup_ms,
           (unsigned)model.cap, warming ? " (warming up)" : "",
           (unsigned long)warmups, (unsigned long)model.st_flushed);
    printf("  Entry path:     capture armed by edge IRQ=%lu, by main loop=%lu | PIO FIFO already full at entry=%lu\n",
           (unsigned long)entries_irq_armed, (unsigned long)entries_main_armed,
           (unsigned long)entries_fifo_full);
    printf("  Edge->session (us): <10:%lu <25:%lu <50:%lu <100:%lu <250:%lu >=250:%lu | avg=%lu max=%lu\n",
           (unsigned long)lat_hist[0], (unsigned long)lat_hist[1], (unsigned long)lat_hist[2],
           (unsigned long)lat_hist[3], (unsigned long)lat_hist[4], (unsigned long)lat_hist[5],
           (unsigned long)(lat_n ? lat_sum / lat_n : 0u), (unsigned long)lat_max);
    printf("  DAC ring:       %lu frames queued | dropped(ring full)=%lu | underruns=%lu\n",
           (unsigned long)subsys_dac_ring_queued_frames(),
           (unsigned long)subsys_dac_dss_drops(),
           (unsigned long)subsys_dac_underruns());
}

// ESC inside a session prints the DSS/DAC counters only -- no beep, no
// subsystem reset, so a game that is mid-sound is left alone.
static void poll_console(void) {
    int c = getchar_timeout_us(0);
    if (c == (int)KEY_ESC) {
        printf("\n");
        subsys_dss_dump_diagnostics();
        printf("\n");
        stdio_flush();
    }
}

/* The strobe loop. Everything a Sound Source program is waiting on happens
 * here: strobes out of the PIO into the FIFO model and the nAck flag back out
 * -- the program re-reads nAck a couple of microseconds after each strobe, so
 * this path is kept as short and as predictable as it can be:
 *   - the function and everything it calls (model, DAC push) runs from RAM,
 *     so no flash cache miss can stall it;
 *   - nAck is driven with an inline GPIO write, not a call;
 *   - the drain-tick work (64-bit time, model advance, DAC push) runs only
 *     when a tick is actually due, found with one 32-bit compare, instead of
 *     on every pass. A pass with nothing to do is a few instructions;
 *   - the (flash-resident, slow) console poll is left to the caller: this
 *     returns when the signature drops or the console deadline arrives.
 */
static inline __attribute__((always_inline)) uint64_t dss_time_us_64(void) {
    uint32_t hi = timer_hw->timerawh;
    uint32_t lo;
    for (;;) {
        lo = timer_hw->timerawl;
        uint32_t hi2 = timer_hw->timerawh;
        if (hi2 == hi) break;
        hi = hi2;
    }
    return ((uint64_t)hi << 32) | lo;
}

/* noinline: the compiler would otherwise fold this single-caller function back
 * into subsys_dss_run_session, i.e. back into flash. */
static __attribute__((noinline)) void __not_in_flash_func(dss_hot_loop)(uint32_t console_deadline_us32) {
    uint8_t  out[ADVANCE_BATCH];
    uint32_t next_due = dss_model_next_due_us32(&model);
    const uint32_t sig_mask = (1u << WIRE_GPIO_NAUTOLF) | (1u << WIRE_GPIO_NINIT);
    const uint32_t p17_mask = 1u << WIRE_GPIO_SELECT_IN;

    for (;;) {
        /* One atomic read of all inputs: a game's single OUT moves both
         * signature lines at once. */
        uint32_t g = sio_hw->gpio_in;
        if ((g & sig_mask) != sig_mask) {
            return;
        }
        uint32_t now = timer_hw->timerawl;

        /* 0. Power cycles (pin 17 low for a while, then high) and the end
         *    of the warm-up window. Both only matter when a deeper-than-16
         *    FIFO is configured. */
        if (dss_power_update(&power, now, (g & p17_mask) != 0u)) {
            if (cfg.depth > DSS_FIFO_DEPTH && cfg.warmup_ms != 0u) {
                warming = true;
                warm_until = now + (uint32_t)cfg.warmup_ms * 1000u;
                warmups++;
                dss_model_set_depth(&model, DSS_FIFO_DEPTH);
                gpio_put(WIRE_GPIO_NACK, dss_model_full(&model));
            }
        }
        if (warming && (int32_t)(now - warm_until) >= 0) {
            warming = false;
            dss_model_set_depth(&model, cfg.depth);
            gpio_put(WIRE_GPIO_NACK, dss_model_full(&model));
        }

        /* 1. Strobes the PIO has latched. nAck follows the model per byte. */
        while (!pio_sm_is_rx_fifo_empty(PIO_DSS, pio_sm_idx)) {
            uint8_t b = (uint8_t)pio_sm_get(PIO_DSS, pio_sm_idx);
            total_captured++;
            dss_model_write(&model, b);
            gpio_put(WIRE_GPIO_NACK, dss_model_full(&model));
        }

        /* 2. Drain clock -> DAC, only when a tick is due. */
        now = timer_hw->timerawl;
        if ((int32_t)(now - next_due) >= 0) {
            uint32_t n = dss_model_advance(&model, dss_time_us_64(), out, ADVANCE_BATCH);
            next_due = dss_model_next_due_us32(&model);
            if (n) {
                subsys_dac_dss_push(out, n);
                gpio_put(WIRE_GPIO_NACK, dss_model_full(&model));
            }
        }

        /* 3. Back to the caller for the occasional console check. */
        if ((int32_t)(now - console_deadline_us32) >= 0) {
            return;
        }
    }
}

/* A deeper-than-16 FIFO is only worth anything if its contents keep playing
 * while the dispatcher owns the port. The strobe loop is not running then, so
 * hand them to the DAC ring now: the I2S DMA plays them out on its own clock.
 * Leaves headroom in the ring; whatever doesn't fit stays queued. */
static void flush_fifo_to_dac(void) {
    uint8_t buf[64];

    if (model.cap <= DSS_FIFO_DEPTH) {
        return;
    }
    while (model.level > 0u) {
        uint32_t room = subsys_dac_ring_free_frames();
        if (room <= FLUSH_RING_HEADROOM) {
            break;
        }
        uint32_t want = room - FLUSH_RING_HEADROOM;
        if (want > sizeof buf) want = sizeof buf;
        uint32_t n = dss_model_flush(&model, buf, want);
        if (n == 0u) {
            break;
        }
        subsys_dac_dss_push(buf, n);
    }
}

void subsys_dss_run_session(void) {
    if (!initialised) {
        return;
    }

    /* Capture first. A game strobes within a few microseconds of the
     * control write that made the signature appear. Normally the edge
     * interrupt has already armed the capture (dss_edge_hook) and whatever
     * the PIO has caught since is real strobes: leave it alone. Set
     * session_active BEFORE looking at armed_by_irq so the hook can't
     * re-arm (and flush) underneath us. If the interrupt didn't arm it, or
     * did so long enough ago to be stale, arm it now as before. */
    session_active = true;
    uint32_t t_sess = timer_hw->timerawl;
    bool pre_armed = armed_by_irq && ((uint32_t)(t_sess - armed_t0) < PRE_ARM_MAX_AGE_US);
    armed_by_irq = false;
    if (pre_armed) {
        entries_irq_armed++;
    } else {
        entries_main_armed++;
        pio_sm_set_enabled(PIO_DSS, pio_sm_idx, false);
        pio_sm_clear_fifos(PIO_DSS, pio_sm_idx);
        pio_sm_restart(PIO_DSS, pio_sm_idx);
        pio_sm_exec(PIO_DSS, pio_sm_idx, pio_encode_jmp(pio_prog_offset));
        pio_sm_set_enabled(PIO_DSS, pio_sm_idx, true);
    }
    if (pio_sm_is_rx_fifo_full(PIO_DSS, pio_sm_idx)) {
        entries_fifo_full++;               /* the SM stalled before we got here */
    }
    {
        uint32_t d = t_sess - wire_last_edge_us();
        if (d < 5000u) {                   /* ignore entries with no fresh edge */
            lat_record(d);
        }
    }

    activity_begin();

    /* The power-up OUT that starts a session also puts a rising edge on
     * nAuto-LF, so the reset IRQ has set (or, if we got here before its
     * handler ran, is about to set) reset_pending. Give a late IRQ time
     * to land, then clear it, or the end of the session would be taken
     * for a reset and flush the DAC ring. */
    busy_wait_us(10);
    wire_clear_reset_pending();

    subsys_dac_dss_begin(DSS_RATE_HZ);

    /* Which FIFO depth does this program get? The configured one -- unless it
     * has just powered the unit up (away for a long time), in which case the
     * real 16 bytes for the warm-up window, so a presence test that overruns
     * the FIFO and expects "full" still passes. A fresh power-up also starts
     * with an empty FIFO. See dss_entry_depth(). */
    {
        bool warm = false;
        uint16_t depth = dss_entry_depth(&cfg, have_exit, (uint32_t)(t_sess - last_exit_us), &warm);
        if (warm) {
            model.level = 0;
            model.head = 0;
            warm_until = t_sess + (uint32_t)cfg.warmup_ms * 1000u;
            warmups++;
        }
        warming = warm;
        dss_model_set_depth(&model, depth);
        power.low = false;
    }

    /* Play out what a real unit would have played while the dispatcher had
     * the port (usually the whole FIFO: it empties in 2.3 ms), THEN report
     * the FIFO state. Re-anchoring without draining would leave it "full"
     * after every disk chunk and the game's first burst would only get a
     * byte or two in. See dss_model_resume_drain(). */
    uint64_t t_start = time_us_64();
    uint8_t  gap[64];
    uint32_t gap_n = dss_model_resume_drain(&model, t_start, gap, sizeof gap);
    if (gap_n) {
        subsys_dac_dss_push(gap, gap_n);
    }
    wire_set_nack(dss_model_full(&model));
    session_count++;

    if (debug_enabled) {
        debug_printf("DSS: session %lu start\n", (unsigned long)session_count);
    }

    uint32_t console_deadline = timer_hw->timerawl + CONSOLE_POLL_US;

    while (wire_dss_signature()) {
        dss_hot_loop(console_deadline);

        uint32_t now32 = timer_hw->timerawl;
        if ((int32_t)(now32 - console_deadline) >= 0) {
            console_deadline = now32 + CONSOLE_POLL_US;
            poll_console();
        }
    }

    /* Session over. */
    session_active = false;
    armed_by_irq = false;
    pio_sm_set_enabled(PIO_DSS, pio_sm_idx, false);
    flush_fifo_to_dac();                    /* deep FIFO only: keep it playing through the gap */
    last_exit_us = timer_hw->timerawl;
    have_exit = true;
    warming = false;
    subsys_dac_dss_end();
    wire_park_outputs();                    /* nAck back to LOW, Error/nibbles idle */
    last_session_us = (uint32_t)(time_us_64() - t_start);

    /* Left by dropping nInit while nAuto-LF stayed high? That is a host
     * reset (control 00h), not a return to idle: re-arm the flag so
     * main.c runs the normal reset sequence next. */
    if (wire_reset_line_high()) {
        wire_request_reset();
    }

    if (debug_enabled) {
        debug_printf("DSS: session end after %lu ms: accepted=%lu overflow=%lu played=%lu held=%lu\n",
                     (unsigned long)(last_session_us / 1000u),
                     (unsigned long)model.st_accepted, (unsigned long)model.st_overflow,
                     (unsigned long)model.st_played, (unsigned long)model.st_held);
    }
}


/* ---------------------------------------------------------------------
 * DISPATCH_CMD_DSS: FIFO depth configuration (see subsys_dss.h; the wire
 * protocol itself is in dss_cmd.c so it can be unit-tested on a host)
 * --------------------------------------------------------------------- */
bool subsys_dss_cmd_run(void) {
    return dss_cmd_run(&cfg);
}
