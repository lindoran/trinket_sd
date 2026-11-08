/*
 * subsys_dac.c -- I2S Audio DAC subsystem implementation for PCM5102.
 */

#include "subsys_dac.h"

#include <stdio.h>
#include <math.h>
#include <string.h>

#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "hardware/dma.h"
#include "hardware/pwm.h"
#include "hardware/clocks.h"
#include "hardware/irq.h"
#include "hardware/structs/nvic.h"

#include "debug.h"
#include "wire.h"
#include "audio_i2s.pio.h"

// ---------------------------------------------------------------------
// Pin definitions (SOUND.md)
// ---------------------------------------------------------------------
#define PIN_I2S_BCK       16   // Bit Clock
#define PIN_I2S_LRCK      17   // Left/Right Word Select
#define PIN_I2S_DIN       27   // Serial Data In
#define PIN_I2S_SCLK      26   // System Master Clock (GPIO / PWM)

#define PIO_AUDIO         pio0
#define DMA_CHUNK_SAMPLES 256u // 256 stereo samples (1024 bytes) per DMA block
#define DAC_RING_BUF_SIZE 16384u // 16KB circular buffer for streaming PCM

// ---------------------------------------------------------------------
// Internal state
// ---------------------------------------------------------------------
static uint pio_sm = 0;
static uint pio_prog_offset = 0;
static int dma_chan_0 = -1;
static int dma_chan_1 = -1;

static uint32_t dma_buf_0[DMA_CHUNK_SAMPLES];
static uint32_t dma_buf_1[DMA_CHUNK_SAMPLES];

static uint8_t ring_buf[DAC_RING_BUF_SIZE];
static volatile uint32_t ring_head = 0;
static volatile uint32_t ring_tail = 0;
static volatile uint32_t ring_count = 0;

static uint32_t current_sample_rate = 44100u;
static uint8_t  current_channels = 2u;
static uint8_t  current_bits = 16u;
static uint8_t  current_sclk_mode = DAC_SCLK_INTERNAL;

static volatile uint8_t  dac_state = DAC_STATE_IDLE;
static volatile uint32_t dac_underruns = 0;
static volatile uint32_t dac_frames_played = 0;
static volatile bool     stream_active = false;

// Test Tone Generator (Bring-up Goals 4 & 5)
static int16_t sine_lut[256];
static volatile bool     tone_active = false;
static volatile uint32_t tone_remaining_samples = 0;
static volatile uint32_t tone_phase_acc = 0;
static volatile uint32_t tone_phase_step = 0;

static bool debug_enabled = false;

void subsys_dac_set_debug(bool enabled) {
    debug_enabled = enabled;
}

// ---------------------------------------------------------------------
// SCLK generation on GPIO26
// ---------------------------------------------------------------------
static void dac_apply_sclk_mode(uint8_t mode, uint32_t sample_rate) {
    current_sclk_mode = mode;
    uint slice = pwm_gpio_to_slice_num(PIN_I2S_SCLK);

    if (mode == DAC_SCLK_EXTERNAL) {
        // Output 256*Fs square wave clock via PWM
        uint32_t target_mclk = 256u * sample_rate;
        float div = (float)clock_get_hz(clk_sys) / (4.0f * (float)target_mclk);

        gpio_set_function(PIN_I2S_SCLK, GPIO_FUNC_PWM);
        pwm_set_wrap(slice, 3);                         // 4 counts per period
        pwm_set_chan_level(slice, PWM_CHAN_A, 2);       // 50% duty cycle
        pwm_set_clkdiv(slice, div);
        pwm_set_enabled(slice, true);

        if (debug_enabled) {
            debug_printf("DAC: SCLK external mode on GPIO26 (MCLK=%lu Hz, div=%.2f)\n",
                         (unsigned long)target_mclk, div);
        }
    } else {
        // Mode 0: SCLK pulled LOW -> PCM5102 internal PLL generates MCLK from BCK
        pwm_set_enabled(slice, false);
        gpio_init(PIN_I2S_SCLK);
        gpio_set_dir(PIN_I2S_SCLK, GPIO_OUT);
        gpio_put(PIN_I2S_SCLK, 0);

        if (debug_enabled) {
            debug_printf("DAC: SCLK internal PLL mode (GPIO26 held LOW)\n");
        }
    }
}

// ---------------------------------------------------------------------
// DMA Buffer Refill (called from ISR and at start)
// ---------------------------------------------------------------------
static void fill_dma_chunk(uint32_t *dest, uint count) {
    if (tone_active) {
        // Built-in test tone generation (24000 amplitude for clear audibility)
        for (uint i = 0; i < count; i++) {
            if (tone_remaining_samples > 0) {
                tone_phase_acc += tone_phase_step;
                uint8_t idx = (uint8_t)(tone_phase_acc >> 24);
                int16_t sample = sine_lut[idx];
                uint16_t s_u16 = (uint16_t)sample;
                dest[i] = ((uint32_t)s_u16 << 16) | (uint32_t)s_u16; // Stereo L+R
                tone_remaining_samples--;
            } else {
                dest[i] = 0; // Silence
                tone_active = false;
                dac_state = DAC_STATE_IDLE;
            }
        }
    } else if (ring_count >= count * 4u) {
        // Stream from circular buffer (each stereo frame is 4 bytes: 2 Left, 2 Right)
        for (uint i = 0; i < count; i++) {
            uint8_t b0 = ring_buf[ring_tail];
            uint8_t b1 = ring_buf[(ring_tail + 1) % DAC_RING_BUF_SIZE];
            uint8_t b2 = ring_buf[(ring_tail + 2) % DAC_RING_BUF_SIZE];
            uint8_t b3 = ring_buf[(ring_tail + 3) % DAC_RING_BUF_SIZE];
            ring_tail = (ring_tail + 4) % DAC_RING_BUF_SIZE;

            uint16_t left  = (uint16_t)b0 | ((uint16_t)b1 << 8);
            uint16_t right = (uint16_t)b2 | ((uint16_t)b3 << 8);
            dest[i] = ((uint32_t)left << 16) | (uint32_t)right;
        }
        ring_count -= count * 4u;
    } else {
        // Idle / Starvation: output silence to keep I2S clocks locked
        memset(dest, 0, count * sizeof(uint32_t));
        if (stream_active && dac_state == DAC_STATE_PLAYING) {
            dac_underruns++;
        }
    }
}

static void __isr dma_irq_handler(void) {
    if (dma_channel_get_irq0_status(dma_chan_0)) {
        dma_channel_acknowledge_irq0(dma_chan_0);
        fill_dma_chunk(dma_buf_0, DMA_CHUNK_SAMPLES);
        dma_channel_set_read_addr(dma_chan_0, dma_buf_0, false);
        dma_channel_set_trans_count(dma_chan_0, DMA_CHUNK_SAMPLES, false);
        dac_frames_played += DMA_CHUNK_SAMPLES;
    }

    if (dma_channel_get_irq0_status(dma_chan_1)) {
        dma_channel_acknowledge_irq0(dma_chan_1);
        fill_dma_chunk(dma_buf_1, DMA_CHUNK_SAMPLES);
        dma_channel_set_read_addr(dma_chan_1, dma_buf_1, false);
        dma_channel_set_trans_count(dma_chan_1, DMA_CHUNK_SAMPLES, false);
        dac_frames_played += DMA_CHUNK_SAMPLES;
    }
}

// ---------------------------------------------------------------------
// Playback control
// ---------------------------------------------------------------------
static void dac_start_playback(void) {
    // Pre-fill both ping-pong buffers
    fill_dma_chunk(dma_buf_0, DMA_CHUNK_SAMPLES);
    fill_dma_chunk(dma_buf_1, DMA_CHUNK_SAMPLES);

    // Arm DMA channels
    dma_channel_set_read_addr(dma_chan_0, dma_buf_0, false);
    dma_channel_set_trans_count(dma_chan_0, DMA_CHUNK_SAMPLES, false);

    dma_channel_set_read_addr(dma_chan_1, dma_buf_1, false);
    dma_channel_set_trans_count(dma_chan_1, DMA_CHUNK_SAMPLES, false);

    // Enable PIO state machine
    pio_sm_set_enabled(PIO_AUDIO, pio_sm, true);

    // Trigger first DMA channel (chains automatically to chan 1)
    dma_channel_start(dma_chan_0);

    dac_state = DAC_STATE_PLAYING;
    if (debug_enabled) {
        debug_printf("DAC: playback started (sample_rate=%lu)\n", (unsigned long)current_sample_rate);
    }
}

static void dac_stop_playback(void) {
    dma_channel_abort(dma_chan_0);
    dma_channel_abort(dma_chan_1);
    pio_sm_set_enabled(PIO_AUDIO, pio_sm, false);

    dac_state = DAC_STATE_IDLE;
    tone_active = false;
    stream_active = false;

    if (debug_enabled) {
        debug_printf("DAC: playback stopped\n");
    }
}

// ---------------------------------------------------------------------
// Subsystem lifecycle
// ---------------------------------------------------------------------
void subsys_dac_init(void) {
    // Generate sine LUT for test tones (amplitude 24000 = ~75% full scale)
    for (int i = 0; i < 256; i++) {
        float angle = (float)i * (2.0f * 3.141592653589793f / 256.0f);
        sine_lut[i] = (int16_t)(sinf(angle) * 24000.0f);
    }

    // Default SCLK to internal PLL mode (GPIO26 LOW)
    dac_apply_sclk_mode(DAC_SCLK_INTERNAL, current_sample_rate);

    // Initialize PIO
    pio_prog_offset = pio_add_program(PIO_AUDIO, &audio_i2s_program);
    audio_i2s_program_init(PIO_AUDIO, pio_sm, pio_prog_offset, PIN_I2S_DIN, PIN_I2S_BCK);
    audio_i2s_set_sample_rate(PIO_AUDIO, pio_sm, current_sample_rate);

    // Claim DMA channels
    dma_chan_0 = dma_claim_unused_channel(true);
    dma_chan_1 = dma_claim_unused_channel(true);

    // Configure DMA Channel 0
    dma_channel_config c0 = dma_channel_get_default_config(dma_chan_0);
    channel_config_set_transfer_data_size(&c0, DMA_SIZE_32);
    channel_config_set_read_increment(&c0, true);
    channel_config_set_write_increment(&c0, false);
    channel_config_set_dreq(&c0, pio_get_dreq(PIO_AUDIO, pio_sm, true));
    channel_config_set_chain_to(&c0, dma_chan_1);
    dma_channel_configure(dma_chan_0, &c0, &PIO_AUDIO->txf[pio_sm], dma_buf_0, DMA_CHUNK_SAMPLES, false);

    // Configure DMA Channel 1
    dma_channel_config c1 = dma_channel_get_default_config(dma_chan_1);
    channel_config_set_transfer_data_size(&c1, DMA_SIZE_32);
    channel_config_set_read_increment(&c1, true);
    channel_config_set_write_increment(&c1, false);
    channel_config_set_dreq(&c1, pio_get_dreq(PIO_AUDIO, pio_sm, true));
    channel_config_set_chain_to(&c1, dma_chan_0);
    dma_channel_configure(dma_chan_1, &c1, &PIO_AUDIO->txf[pio_sm], dma_buf_1, DMA_CHUNK_SAMPLES, false);

    // Set up DMA IRQ
    dma_channel_set_irq0_enabled(dma_chan_0, true);
    dma_channel_set_irq0_enabled(dma_chan_1, true);
    irq_set_exclusive_handler(DMA_IRQ_0, dma_irq_handler);
    irq_set_enabled(DMA_IRQ_0, true);

    ring_head = 0;
    ring_tail = 0;
    ring_count = 0;
    dac_state = DAC_STATE_IDLE;
    dac_underruns = 0;
    dac_frames_played = 0;
    tone_active = false;
    stream_active = false;

    // Start continuous hardware I2S clocks with silence so PCM5102 PLL locks immediately
    dac_start_playback();
    dac_state = DAC_STATE_IDLE;
}

void subsys_dac_reset(void) {
    ring_head = 0;
    ring_tail = 0;
    ring_count = 0;
    tone_active = false;
    stream_active = false;
    dac_state = DAC_STATE_IDLE;

    dac_apply_sclk_mode(DAC_SCLK_INTERNAL, current_sample_rate);
    audio_i2s_set_sample_rate(PIO_AUDIO, pio_sm, current_sample_rate);
}

void subsys_dac_service(void) {
    // Non-blocking background hook called between dispatch / SD sector transfers.
}

// ---------------------------------------------------------------------
// Command handlers (Write direction)
// ---------------------------------------------------------------------
static bool recv_next_byte(uint8_t *out) {
    if (!wire_wait_init_high()) {
        return false;
    }
    return wire_finish_write_handshake(out);
}

static bool handle_op_config(void) {
    uint8_t buf[7];
    for (int i = 0; i < 7; i++) {
        if (!recv_next_byte(&buf[i])) return false;
    }

    uint32_t rate = (uint32_t)buf[0] | ((uint32_t)buf[1] << 8) |
                    ((uint32_t)buf[2] << 16) | ((uint32_t)buf[3] << 24);
    uint8_t channels = buf[4];
    uint8_t bits = buf[5];
    uint8_t sclk_mode = buf[6];

    if (rate >= 4000u && rate <= 192000u) {
        current_sample_rate = rate;
    }
    if (channels == 1 || channels == 2) {
        current_channels = channels;
    }
    current_bits = bits ? bits : 16u;

    audio_i2s_set_sample_rate(PIO_AUDIO, pio_sm, current_sample_rate);
    dac_apply_sclk_mode(sclk_mode, current_sample_rate);

    if (debug_enabled) {
        debug_printf("DAC: CONFIG rate=%lu channels=%u bits=%u sclk_mode=%u\n",
                     (unsigned long)current_sample_rate, (unsigned)current_channels,
                     (unsigned)current_bits, (unsigned)current_sclk_mode);
    }
    return true;
}

static bool handle_op_start(void) {
    stream_active = true;
    dac_start_playback();
    return true;
}

// Empties the ring buffer immediately, discarding any audio queued but
// not yet played. Added 2026-09-14 -- the DOS-side Wolf3D
// SDL_SSStopSample() fix found this the hard way: every existing
// caller of DAC_OP_STOP wants playback to stop AND any not-yet-played
// queued audio discarded (a retriggered sound interrupting a
// still-playing one, or a full shutdown) -- nothing in this codebase
// ever wanted "pause, keep the buffered audio, resume later" (STOP was
// never called with that intent anywhere in dactest.c, WL/ID_SD.C's
// trinket client, or this file's own subsys_dac_reset(), which already
// zeroes ring_head/tail/count on a full protocol reset -- this just
// makes DAC_OP_STOP consistent with that existing precedent instead of
// being the one place that silently left stale audio queued).
// Deliberately NOT a separate new opcode: a caller wanting today's old
// "stop but keep buffered data for a later resume" behavior doesn't
// exist anywhere in this tree, so a second op would add protocol
// surface area (and a DOS-side wire round trip) for a use case nobody
// has. If a real pause/resume need shows up later, add a distinct
// DAC_OP_PAUSE then rather than overloading this again.
//
// Runs with the DMA IRQ masked: dma_channel_abort() (called by
// dac_stop_playback() just before this) is synchronous/blocking per
// the SDK, but an IRQ0 status bit that had already latched for a chunk
// completing right before the abort took hold could still fire
// dma_irq_handler() once after we return from it -- and that handler's
// fill_dma_chunk() reads ring_tail/ring_count. Masking here means that
// straggler (if it happens at all) just finds an empty ring and falls
// into the existing silence/idle path in fill_dma_chunk() instead of
// racing this reset.
static void dac_flush_ring(void) {
    irq_set_enabled(DMA_IRQ_0, false);
    ring_head = 0;
    ring_tail = 0;
    ring_count = 0;
    irq_set_enabled(DMA_IRQ_0, true);
}

static bool handle_op_stop(void) {
    dac_stop_playback();
    dac_flush_ring();
    return true;
}

static bool handle_op_status(void) {
    return true;
}

// Blocks until the ring has room for `needed` more bytes, or bails out
// if the host asserts a reset while we're waiting. Safe to busy-wait
// on: dac_start_playback() is always running from boot (subsys_dac_init()
// starts the DMA/PIO loop unconditionally, even while idle it drains the
// ring into silence-or-audio every DMA block), so ring_count is always
// being consumed in the background regardless of what this core is
// doing -- this just waits for that consumer to catch up instead of
// silently discarding PCM the way the original code did.
static bool wait_for_ring_space(uint32_t needed) {
    while (ring_count + needed > DAC_RING_BUF_SIZE) {
        if (wire_reset_pending()) {
            return false;
        }
        tight_loop_contents();
    }
    return true;
}

// Pushes one signed 16-bit stereo frame (left, right) into the ring
// buffer, LE byte order per channel -- same layout fill_dma_chunk()
// already expects. Blocks on wait_for_ring_space() same as the
// pre-existing per-byte paths below. Returns false only if a host
// reset interrupts the wait. Shared by both 8-bit paths in
// handle_op_write() so the ring-packing logic exists once instead of
// twice.
static bool push_stereo_frame16(int16_t left, int16_t right) {
    uint16_t l, r;

    if (!wait_for_ring_space(4u)) return false;

    l = (uint16_t)left;
    r = (uint16_t)right;

    ring_buf[ring_head] = (uint8_t)(l & 0xFFu);
    ring_buf[(ring_head + 1) % DAC_RING_BUF_SIZE] = (uint8_t)((l >> 8) & 0xFFu);
    ring_buf[(ring_head + 2) % DAC_RING_BUF_SIZE] = (uint8_t)(r & 0xFFu);
    ring_buf[(ring_head + 3) % DAC_RING_BUF_SIZE] = (uint8_t)((r >> 8) & 0xFFu);
    ring_head = (ring_head + 4) % DAC_RING_BUF_SIZE;
    ring_count += 4u;
    return true;
}

// Unsigned 8-bit PCM (0..255, midpoint 128 -- the standard WAV/raw
// 8-bit convention) -> signed 16-bit PCM, matching what the rest of
// this pipeline (ring buffer, fill_dma_chunk(), the PCM5102 over I2S)
// already assumes. Re-center on 0, then scale into the high byte:
// -128..127 -> -32768..32512, close enough to the full signed 16-bit
// range that no separate scaling factor is needed. Not interpolated
// or dithered -- just enough to make an 8-bit source audible through
// the existing 16-bit ring/DMA/PIO path without a second, parallel
// DAC code path for a different sample width.
static inline __attribute__((always_inline)) int16_t upsample_8_to_16(uint8_t raw) {
    return (int16_t)(((int)raw - 128) << 8);
}

// ---------------------------------------------------------------------
// Disney Sound Source sink (firmware-internal, see subsys_dac.h)
// ---------------------------------------------------------------------
static volatile uint32_t dss_ring_drops = 0;

// A DSS session counts as "playing" for underrun purposes only once the
// ring has held one full DMA chunk. Until then the DMA IRQ is going to find
// the ring short at least once simply because the audio hasn't had time to
// arrive (a chunk is 36.6 ms of audio at 7 kHz), and counting that would
// bury real mid-stream starvation under one false underrun per session.
static volatile bool dss_primed = false;

void subsys_dac_dss_begin(uint32_t rate_hz) {
    if (rate_hz < 4000u || rate_hz > 192000u) {
        rate_hz = current_sample_rate;
    }
    // Same configuration the Wolf3D client sets through DAC_OP_CONFIG:
    // internal-PLL SCLK (GPIO26 low), rate as given. Channels/bits only
    // describe how a wire client's DAC_OP_WRITE payload is parsed, which
    // this path bypasses, so they are left alone.
    //
    // A DSS session can start and stop many times in one game (every time
    // the dispatcher takes the port for a disk read, then the game writes
    // the control port again), so only touch the I2S clock and the SCLK pin
    // when something actually differs. Re-running gpio_init() on SCLK and
    // rewriting the PIO divider for no reason is exactly the kind of thing
    // that makes a PCM5102 glitch.
    if (rate_hz != current_sample_rate || current_sclk_mode != DAC_SCLK_INTERNAL) {
        current_sample_rate = rate_hz;
        audio_i2s_set_sample_rate(PIO_AUDIO, pio_sm, current_sample_rate);
        dac_apply_sclk_mode(DAC_SCLK_INTERNAL, current_sample_rate);
    }

    dac_state = DAC_STATE_PLAYING;

    // Not counting underruns yet -- see dss_primed above. If audio from a
    // previous session is still queued, the first push below primes at once.
    irq_set_enabled(DMA_IRQ_0, false);
    stream_active = false;
    dss_primed = false;
    irq_set_enabled(DMA_IRQ_0, true);

    bool pio_on = (PIO_AUDIO->ctrl & (1u << pio_sm)) != 0;
    bool dma_on = dma_channel_is_busy(dma_chan_0) || dma_channel_is_busy(dma_chan_1);
    if (!pio_on || !dma_on) {
        dac_start_playback();
    }
}

// Runs once per 7 kHz drain tick from the DSS session loop, so it executes
// out of RAM and never calls into flash: a Sound Source program re-checks
// nAck within a couple of microseconds of a strobe, and this call can land in
// the middle of a burst. The DMA interrupt is masked straight through the
// NVIC registers -- the same operations irq_set_enabled() performs, minus the
// flash-resident call (enabling clears any stale pending bit first; DMA IRQ0
// is level-sensitive, so a chunk that finished meanwhile re-pends at once).
void __not_in_flash_func(subsys_dac_dss_push)(const uint8_t *samples, uint32_t n) {
    const uint32_t dma_irq_mask = 1u << DMA_IRQ_0;

    // The DMA IRQ's fill_dma_chunk() does a read-modify-write on
    // ring_count; mask it while we do ours so neither update is lost.
    nvic_hw->icer = dma_irq_mask;
    for (uint32_t i = 0; i < n; i++) {
        if (ring_count + 4u > DAC_RING_BUF_SIZE) {
            dss_ring_drops++;
            continue;
        }
        uint16_t v = (uint16_t)upsample_8_to_16(samples[i]);
        uint8_t lo = (uint8_t)(v & 0xFFu);
        uint8_t hi = (uint8_t)(v >> 8);
        ring_buf[ring_head] = lo;
        ring_buf[(ring_head + 1) % DAC_RING_BUF_SIZE] = hi;
        ring_buf[(ring_head + 2) % DAC_RING_BUF_SIZE] = lo;   // right = left
        ring_buf[(ring_head + 3) % DAC_RING_BUF_SIZE] = hi;
        ring_head = (ring_head + 4) % DAC_RING_BUF_SIZE;
        ring_count += 4u;
    }
    if (!dss_primed && ring_count >= DMA_CHUNK_SAMPLES * 4u) {
        dss_primed = true;
        stream_active = true;      // from here on a starved chunk is a real underrun
    }
    nvic_hw->icpr = dma_irq_mask;
    nvic_hw->iser = dma_irq_mask;
}

void subsys_dac_dss_end(void) {
    irq_set_enabled(DMA_IRQ_0, false);
    stream_active = false;
    dss_primed = false;
    irq_set_enabled(DMA_IRQ_0, true);
}

uint32_t subsys_dac_dss_drops(void) {
    return dss_ring_drops;
}

uint32_t subsys_dac_ring_queued_frames(void) {
    return ring_count / 4u;
}

uint32_t subsys_dac_ring_free_frames(void) {
    return (DAC_RING_BUF_SIZE - ring_count) / 4u;
}

uint32_t subsys_dac_underruns(void) {
    return dac_underruns;
}

static bool handle_op_write(void) {
    uint8_t len_bytes[2];
    uint16_t len;
    uint16_t i;

    if (!recv_next_byte(&len_bytes[0])) return false;
    if (!recv_next_byte(&len_bytes[1])) return false;

    len = (uint16_t)len_bytes[0] | ((uint16_t)len_bytes[1] << 8);

    if (current_bits == 8u) {
        if (current_channels == 1) {
            // Mono 8-bit PCM: one wire byte per sample -- half the
            // bytes-on-the-wire of the 16-bit mono path below for the
            // same sample count, which is the whole point of this mode
            // (see DACTEST's "play ... 8" option / TOOLCHAIN.md-adjacent
            // notes on isolating wire-bandwidth-limited underruns from
            // some other cause).
            for (i = 0; i < len; i++) {
                uint8_t raw;
                int16_t sample;
                if (!recv_next_byte(&raw)) return false;
                sample = upsample_8_to_16(raw);
                if (!push_stereo_frame16(sample, sample)) return false;
            }
        } else {
            // Stereo 8-bit PCM: one wire byte per channel per frame
            // (left, then right). An odd trailing byte -- a truncated
            // file, unlikely but possible -- is treated as a mono
            // sample for that last frame (right = left) rather than
            // blocking forever on a byte that will never arrive; same
            // "handle a partial frame safely instead of hanging"
            // spirit as the mono 16-bit path's `i + 1 < len` guard.
            for (i = 0; i < len; i += 2) {
                uint8_t left_raw, right_raw;
                int16_t left, right;
                if (!recv_next_byte(&left_raw)) return false;
                right_raw = left_raw;
                if (i + 1 < len) {
                    if (!recv_next_byte(&right_raw)) return false;
                }
                left = upsample_8_to_16(left_raw);
                right = upsample_8_to_16(right_raw);
                if (!push_stereo_frame16(left, right)) return false;
            }
        }
    } else if (current_channels == 1) {
        // Mono 16-bit PCM: convert to stereo frame by duplicating samples
        for (i = 0; i < len; i += 2) {
            uint8_t lo = 0, hi = 0;
            if (!recv_next_byte(&lo)) return false;
            if (i + 1 < len) {
                if (!recv_next_byte(&hi)) return false;
            }

            if (!wait_for_ring_space(4u)) return false;

            // Left channel
            ring_buf[ring_head] = lo;
            ring_buf[(ring_head + 1) % DAC_RING_BUF_SIZE] = hi;
            // Right channel (duplicate)
            ring_buf[(ring_head + 2) % DAC_RING_BUF_SIZE] = lo;
            ring_buf[(ring_head + 3) % DAC_RING_BUF_SIZE] = hi;
            ring_head = (ring_head + 4) % DAC_RING_BUF_SIZE;
            ring_count += 4u;
        }
    } else {
        // Stereo 16-bit PCM
        for (i = 0; i < len; i++) {
            uint8_t byte;
            if (!recv_next_byte(&byte)) return false;

            if (!wait_for_ring_space(1u)) return false;

            ring_buf[ring_head] = byte;
            ring_head = (ring_head + 1) % DAC_RING_BUF_SIZE;
            ring_count++;
        }
    }

    return true;
}

void subsys_dac_play_tone(uint16_t freq_hz, uint16_t duration_ms, uint8_t sclk_mode) {
    if (current_sample_rate == 0) {
        current_sample_rate = 44100u;
    }

    audio_i2s_set_sample_rate(PIO_AUDIO, pio_sm, current_sample_rate);
    dac_apply_sclk_mode(sclk_mode, current_sample_rate);

    // Compute tone phase increment and sample duration
    uint32_t total_samples = ((uint32_t)duration_ms * current_sample_rate) / 1000u;
    uint64_t step_calc = ((uint64_t)freq_hz << 32) / (uint64_t)current_sample_rate;

    tone_phase_step = (uint32_t)step_calc;
    tone_remaining_samples = total_samples;
    tone_phase_acc = 0;
    tone_active = true;
    dac_state = DAC_STATE_PLAYING;

    bool pio_on = (PIO_AUDIO->ctrl & (1u << pio_sm)) != 0;
    bool dma_on = dma_channel_is_busy(dma_chan_0) || dma_channel_is_busy(dma_chan_1);
    if (!pio_on || !dma_on) {
        dac_start_playback();
    }

    if (debug_enabled) {
        debug_printf("DAC: TONE freq=%u Hz duration=%u ms sclk=%u rate=%lu\n",
                     (unsigned)freq_hz, (unsigned)duration_ms, (unsigned)sclk_mode,
                     (unsigned long)current_sample_rate);
    }
}

static bool handle_op_tone(void) {
    uint8_t buf[9];
    for (int i = 0; i < 9; i++) {
        if (!recv_next_byte(&buf[i])) return false;
    }

    uint16_t freq_hz = (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);
    uint16_t duration_ms = (uint16_t)buf[2] | ((uint16_t)buf[3] << 8);
    uint8_t sclk_mode = buf[4];
    uint32_t rate = (uint32_t)buf[5] | ((uint32_t)buf[6] << 8) |
                    ((uint32_t)buf[7] << 16) | ((uint32_t)buf[8] << 24);

    if (rate >= 4000u && rate <= 192000u) {
        current_sample_rate = rate;
    }

    subsys_dac_play_tone(freq_hz, duration_ms, sclk_mode);
    return true;
}

static bool handle_op_reset_metrics(void) {
    dac_underruns = 0;
    dac_frames_played = 0;
    return true;
}

static bool run_write_side(void) {
    uint8_t opcode;
    if (!wire_finish_write_handshake(&opcode)) {
        return false;
    }

    switch (opcode) {
        case DAC_OP_CONFIG:
            return handle_op_config();
        case DAC_OP_START:
            return handle_op_start();
        case DAC_OP_STOP:
            return handle_op_stop();
        case DAC_OP_STATUS:
            return handle_op_status();
        case DAC_OP_WRITE:
            return handle_op_write();
        case DAC_OP_TONE:
            return handle_op_tone();
        case DAC_OP_RESET_METRICS:
            return handle_op_reset_metrics();
        default:
            debug_printf("DAC: unknown opcode 0x%02X\n", opcode);
            return true;
    }
}

// ---------------------------------------------------------------------
// Read-direction: telemetry packet (21 bytes)
// ---------------------------------------------------------------------
static bool send_next_byte(uint8_t byte) {
    uint8_t corrected = byte ^ 0x88u; // doc sec 7 correction for nibble reads
    uint8_t low_nibble  = corrected & 0x0Fu;
    uint8_t high_nibble = (corrected >> 4) & 0x0Fu;

    if (!wire_finish_read_cycle(low_nibble, high_nibble)) return false;
    if (!wire_wait_init_high()) return false;
    return wire_finish_read_cycle(low_nibble, high_nibble);
}

static bool send_subsequent_byte(uint8_t byte) {
    if (!wire_wait_init_high()) return false;
    return send_next_byte(byte);
}

static bool run_read_side(void) {
    uint16_t ring_free = (uint16_t)(DAC_RING_BUF_SIZE - ring_count);
    uint16_t ring_queued = (uint16_t)ring_count;

    // Byte 1: Status
    if (!send_next_byte(DAC_STATUS_OK)) return false;
    // Byte 2: State (0=idle, 1=playing)
    if (!send_subsequent_byte((dac_state == DAC_STATE_PLAYING) ? 1u : 0u)) return false;
    // Bytes 3-6: Sample Rate (LE)
    if (!send_subsequent_byte((uint8_t)(current_sample_rate & 0xFFu))) return false;
    if (!send_subsequent_byte((uint8_t)((current_sample_rate >> 8) & 0xFFu))) return false;
    if (!send_subsequent_byte((uint8_t)((current_sample_rate >> 16) & 0xFFu))) return false;
    if (!send_subsequent_byte((uint8_t)((current_sample_rate >> 24) & 0xFFu))) return false;
    // Byte 7: Channels
    if (!send_subsequent_byte(current_channels)) return false;
    // Byte 8: Bits
    if (!send_subsequent_byte(current_bits)) return false;
    // Byte 9: SCLK mode
    if (!send_subsequent_byte(current_sclk_mode)) return false;
    // Bytes 10-11: Ring free bytes (LE)
    if (!send_subsequent_byte((uint8_t)(ring_free & 0xFFu))) return false;
    if (!send_subsequent_byte((uint8_t)((ring_free >> 8) & 0xFFu))) return false;
    // Bytes 12-13: Ring queued bytes (LE)
    if (!send_subsequent_byte((uint8_t)(ring_queued & 0xFFu))) return false;
    if (!send_subsequent_byte((uint8_t)((ring_queued >> 8) & 0xFFu))) return false;
    // Bytes 14-17: Underrun count (LE)
    if (!send_subsequent_byte((uint8_t)(dac_underruns & 0xFFu))) return false;
    if (!send_subsequent_byte((uint8_t)((dac_underruns >> 8) & 0xFFu))) return false;
    if (!send_subsequent_byte((uint8_t)((dac_underruns >> 16) & 0xFFu))) return false;
    if (!send_subsequent_byte((uint8_t)((dac_underruns >> 24) & 0xFFu))) return false;
    // Bytes 18-21: Frames played (LE)
    if (!send_subsequent_byte((uint8_t)(dac_frames_played & 0xFFu))) return false;
    if (!send_subsequent_byte((uint8_t)((dac_frames_played >> 8) & 0xFFu))) return false;
    if (!send_subsequent_byte((uint8_t)((dac_frames_played >> 16) & 0xFFu))) return false;
    if (!send_subsequent_byte((uint8_t)((dac_frames_played >> 24) & 0xFFu))) return false;

    return true;
}

bool subsys_dac_run(void) {
    if (!wire_wait_init_high()) {
        return false;
    }

    if (wire_is_write_selected()) {
        return run_write_side();
    }
    return run_read_side();
}

void subsys_dac_dump_diagnostics(void) {
    printf("[I2S Audio DAC Subsystem Status (PCM5102)]\n");
    printf("  GPIO16 (BCK):   %u (Bit Clock, func %u)\n", gpio_get(PIN_I2S_BCK), gpio_get_function(PIN_I2S_BCK));
    printf("  GPIO17 (LRCK):  %u (Left/Right Word Select, func %u)\n", gpio_get(PIN_I2S_LRCK), gpio_get_function(PIN_I2S_LRCK));
    printf("  GPIO27 (DIN):   %u (Data Out to DAC DIN, func %u)\n", gpio_get(PIN_I2S_DIN), gpio_get_function(PIN_I2S_DIN));
    printf("  GPIO26 (SCLK):  %u (Master Clock, func %u) -> Mode: %s\n",
           gpio_get(PIN_I2S_SCLK), gpio_get_function(PIN_I2S_SCLK),
           (current_sclk_mode == DAC_SCLK_EXTERNAL) ? "EXTERNAL (256*Fs PWM)" : "INTERNAL PLL (GPIO26 LOW)");

    bool pio_enabled = (PIO_AUDIO->ctrl & (1u << pio_sm)) != 0;
    uint pc = pio_sm_get_pc(PIO_AUDIO, pio_sm);
    uint tx_fifo = pio_sm_get_tx_fifo_level(PIO_AUDIO, pio_sm);
    printf("  PIO0 SM%u:      %s (PC: 0x%02X, TX FIFO Level: %u/8)\n",
           pio_sm, pio_enabled ? "RUNNING" : "STOPPED", pc, tx_fifo);

    if (dma_chan_0 >= 0 && dma_chan_1 >= 0) {
        bool b0 = dma_channel_is_busy(dma_chan_0);
        bool b1 = dma_channel_is_busy(dma_chan_1);
        uint32_t tc0 = dma_channel_hw_addr(dma_chan_0)->transfer_count;
        uint32_t tc1 = dma_channel_hw_addr(dma_chan_1)->transfer_count;
        printf("  DMA Chan %d:    Busy: %s | Remaining Count: %lu\n", dma_chan_0, b0 ? "YES" : "NO", (unsigned long)tc0);
        printf("  DMA Chan %d:    Busy: %s | Remaining Count: %lu\n", dma_chan_1, b1 ? "YES" : "NO", (unsigned long)tc1);
    }

    printf("  Engine State:   %s | %lu Hz | %u Channels | %u-bit\n",
           (dac_state == DAC_STATE_PLAYING) ? "PLAYING" : "IDLE",
           (unsigned long)current_sample_rate, (unsigned)current_channels, (unsigned)current_bits);
    printf("  Ring Buffer:    %lu bytes queued (%lu bytes free)\n",
           (unsigned long)ring_count, (unsigned long)(DAC_RING_BUF_SIZE - ring_count));
    printf("  Audio Metrics:  Frames Played: %lu | Buffer Underruns: %lu\n",
           (unsigned long)dac_frames_played, (unsigned long)dac_underruns);
    printf("  Tone Generator: Active: %s (Remaining Samples: %lu)\n",
           tone_active ? "YES" : "NO", (unsigned long)tone_remaining_samples);

    printf("\n  [PCM5102 Breakout Wiring & Hardware Checklist]\n");
    printf("    * VIN  -> RP2040 3.3V (Pin 36 / 3V3_OUT)\n");
    printf("    * GND  -> RP2040 GND\n");
    printf("    * BCK  -> RP2040 GPIO16\n");
    printf("    * LRCK -> RP2040 GPIO17\n");
    printf("    * DIN  -> RP2040 GPIO27\n");
    printf("    * SCLK -> RP2040 GPIO26 (held LOW for internal PLL mode)\n");
    printf("    * XMT  -> MUST be connected to 3.3V (HIGH) or Jumper 3 soldered to 'H'.\n");
    printf("              (WARNING: If XMT is floating or LOW, PCM5102 hard-mutes audio!)\n");
    printf("    * FMT  -> Connect to GND (I2S standard Philips format)\n");
    printf("    * FLT  -> Connect to GND (Normal latency filter)\n");
    printf("    * DMP  -> Connect to GND (De-emphasis off)\n");
}
