/*
 * dss_model.c -- see dss_model.h.
 */

#include "dss_model.h"

#include <string.h>

void dss_model_init(dss_model_t *m, uint32_t rate_hz) {
    if (rate_hz == 0u) {
        rate_hz = DSS_RATE_HZ;
    }
    memset(m, 0, sizeof(*m));
    /* period in microseconds = 1e6 / rate, kept in Q16 so 7000 Hz
     * (142.857... us) doesn't accumulate integer-rounding error. */
    m->period_q16 = (uint32_t)((((uint64_t)1000000u) << 16) / rate_hz);
    m->hold = DSS_HOLD_INIT;
    m->cap = DSS_FIFO_DEPTH;
}

void dss_model_resume(dss_model_t *m, uint64_t now_us) {
    m->next_q16 = (now_us << 16) + m->period_q16;
    m->anchored = 1u;
}

uint32_t dss_model_resume_drain(dss_model_t *m, uint64_t now_us,
                                uint8_t *out, uint32_t out_max) {
    uint32_t n = 0;

    if (m->anchored) {
        uint64_t now_q16 = now_us << 16;
        if (now_q16 >= m->next_q16) {
            /* Ticks that fell due while nobody was servicing the model:
             * the one at next_q16 plus one per full period after it. */
            uint64_t due = 1u + (now_q16 - m->next_q16) / m->period_q16;
            while (n < out_max && m->level > 0u && (uint64_t)n < due) {
                m->hold = m->fifo[m->head];
                m->head = (uint16_t)((m->head + 1u) & (DSS_FIFO_MAX - 1u));
                m->level--;
                m->st_played++;
                m->st_gap_drained++;
                out[n++] = m->hold;
            }
        }
    }
    dss_model_resume(m, now_us);
    return n;
}

bool DSS_RAMFUNC(dss_model_write)(dss_model_t *m, uint8_t b) {
    if (m->level >= m->cap) {
        m->st_overflow++;
        return false;
    }
    m->fifo[(uint16_t)((m->head + m->level) & (DSS_FIFO_MAX - 1u))] = b;
    m->level++;
    m->st_accepted++;
    if (m->level > m->st_max_level) {
        m->st_max_level = m->level;
    }
    return true;
}

uint32_t DSS_RAMFUNC(dss_model_advance)(dss_model_t *m, uint64_t now_us,
                                         uint8_t *out, uint32_t out_max) {
    uint64_t now_q16 = now_us << 16;
    uint32_t n = 0;

    if (!m->anchored) {
        dss_model_resume(m, now_us);
        return 0;
    }

    while (n < out_max && now_q16 >= m->next_q16) {
        if (m->level > 0u) {
            m->hold = m->fifo[m->head];
            m->head = (uint16_t)((m->head + 1u) & (DSS_FIFO_MAX - 1u));
            m->level--;
            m->st_played++;
        } else {
            m->st_held++;
        }
        out[n++] = m->hold;
        m->next_q16 += m->period_q16;
    }

    if (now_q16 >= m->next_q16) {
        /* Still behind after filling out[]: drop the lateness rather
         * than let it snowball into an ever-growing backlog. */
        m->next_q16 = now_q16 + m->period_q16;
        m->st_resyncs++;
    }
    return n;
}

void DSS_RAMFUNC(dss_model_set_depth)(dss_model_t *m, uint32_t depth) {
    if (depth < DSS_FIFO_DEPTH) depth = DSS_FIFO_DEPTH;
    if (depth > DSS_FIFO_MAX) depth = DSS_FIFO_MAX;
    m->cap = (uint16_t)depth;
    if (m->level > m->cap) {
        m->level = m->cap;
    }
}

uint32_t dss_model_flush(dss_model_t *m, uint8_t *out, uint32_t out_max) {
    uint32_t n = 0;
    while (n < out_max && m->level > 0u) {
        m->hold = m->fifo[m->head];
        m->head = (uint16_t)((m->head + 1u) & (DSS_FIFO_MAX - 1u));
        m->level--;
        m->st_flushed++;
        out[n++] = m->hold;
    }
    return n;
}

void dss_config_default(dss_config_t *c) {
    c->depth = DSS_FIFO_DEPTH;
    c->warmup_ms = DSS_WARMUP_MS_DEFAULT;
}

bool dss_config_clamp(dss_config_t *c) {
    bool changed = false;
    if (c->depth < DSS_FIFO_DEPTH) { c->depth = DSS_FIFO_DEPTH; changed = true; }
    if (c->depth > DSS_FIFO_MAX)   { c->depth = DSS_FIFO_MAX;   changed = true; }
    if (c->warmup_ms > DSS_WARMUP_MS_MAX) { c->warmup_ms = DSS_WARMUP_MS_MAX; changed = true; }
    return changed;
}

uint16_t dss_entry_depth(const dss_config_t *c, bool had_previous_session,
                         uint32_t absence_us, bool *warming) {
    bool fresh = !had_previous_session || absence_us >= DSS_FRESH_ABSENCE_US;
    *warming = false;
    if (c->depth <= DSS_FIFO_DEPTH) {
        return DSS_FIFO_DEPTH;
    }
    if (fresh && c->warmup_ms > 0u) {
        *warming = true;
        return DSS_FIFO_DEPTH;
    }
    return c->depth;
}
