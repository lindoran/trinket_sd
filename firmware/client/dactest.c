/*
 * dactest.c -- Standalone PCM5102 Audio DAC & Coexistence Diagnostic Tool
 *              for DOS, built for Borland C++ 3.1 (small model).
 *
 * Talks directly to the parallel port and exercises DISPATCH_CMD_DAC (0x04)
 * and DISPATCH_CMD_SD (0x03) over the Trinket LPT wire protocol.
 *
 * Commands:
 *   DACTEST beep [/P=378]
 *     Play a 1000Hz test tone (2000ms) using the internal oscillator (SCLK=0 / GPIO26 low).
 *
 *   DACTEST beep_ext [/P=378]
 *     Play a 1000Hz test tone (2000ms) using the GPIO26 master clock (SCLK=1 / 256*Fs).
 *
 *   DACTEST tone <freq_hz> <duration_ms> [int|ext] [sample_rate] [/P=378]
 *     Play a configurable tone with specified frequency, duration, clock mode, and sample rate.
 *
 *   DACTEST status [/P=378]
 *     Query and display the current DAC engine telemetry (playback state, sample rate,
 *     SCLK mode, buffer capacity, queued bytes, underruns, and frames played).
 *
 *   DACTEST stop [/P=378]
 *     Stop DAC playback and silence the output.
 *
 *   DACTEST play <file.pcm> [sample_rate] [1|2] [int|ext] [8|16] [/P=378]
 *     Stream raw PCM audio from a DOS file to the DAC over the parallel port.
 *     Bit depth defaults to 16; pass 8 for unsigned 8-bit PCM input -- half
 *     the wire bytes per sample, upsampled to 16-bit on the trinket itself
 *     (see subsys_dac.c/subsys_dac.h's DAC_OP_WRITE framing). Added 2026-09
 *     to help isolate whether playback underruns are wire-bandwidth-limited
 *     (a slow/8-bit stream should tolerate a much lower sustained rate
 *     before underrunning than 44.1kHz 16-bit stereo does) or caused by
 *     something else in the pipeline.
 *
 *   DACTEST coex [sectors_per_batch] [iterations] [/P=378]
 *     Test multiplexed coexistence of concurrent SD storage traffic and DAC audio playback
 *     to verify that storage reads do not break audio and audio does not break storage.
 *
 * PORTED FROM ia16-elf-gcc (added back into the BC31-only tree, ported on
 * arrival): this project's DOS toolchain is Borland C++ 3.1 across the
 * board -- see TOOLCHAIN.md. Two categories of change from the
 * ia16-elf-gcc version, neither a logic change:
 *   - outportb()/inportb() (Section 1) now come from Borland's <dos.h>
 *     instead of hand-rolled GCC inline asm -- same single IN/OUT
 *     instruction either way, same as sdtest.c/iftest.c's own port.
 *   - Every `for (TYPE var = ...; ...; ...)` loop-scoped declaration,
 *     and every declaration appearing after a prior statement in the
 *     same block, is hoisted to the top of its enclosing block. Borland
 *     C++ 3.1 predates C99 and rejects both (confirmed directly: a
 *     minimal `for (int i = 0; ...)` under `bcc` fails with "Expression
 *     syntax" / "Undefined symbol 'i'" / "Statement missing ;"). This
 *     file had eleven for-loops and several mid-function declarations
 *     using both forms -- sdtest.c/iftest.c happened to already be
 *     written C89-clean and needed no equivalent fixes, so this is a
 *     real, if mechanical, category of change specific to this file.
 *   - `//` line comments are NOT converted -- confirmed directly that
 *     `bcc` accepts them as a vendor extension even in a plain `.c`
 *     file, same as they already appear throughout this project's
 *     hand-written BC31 driver sources.
 * Untouched: the wire protocol, dispatch framing, DAC/SD command
 * layers, and every command handler's actual logic -- diffed against
 * the ia16 original to confirm no unintended changes beyond the
 * mechanical C89 hoisting above.
 *
 * DWORD arithmetic note: this file reconstructs/decomposes several
 * 32-bit fields byte-at-a-time via shifts (dac_get_status()'s
 * sample_rate/underruns/frames_played, dac_config()/dac_play_tone()/
 * dac_write_chunk()/sd2_read_sectors()'s argument encoding) -- the same
 * shape of `unsigned long` shift/multiply already execution-verified
 * safe under BC31 for sdtest.c's equivalent patterns (see that file's
 * header comment and TOOLCHAIN.md's driver-hazards section: the known
 * BC31 DWORD-shift crash is specific to TRINKET.SYS's private-stack
 * STRATEGY/INTERRUPT calling context, which a plain .EXE like this one
 * never enters). Re-checked here too, not just assumed by analogy --
 * see this project's toolchain notes for the bcc -S / grep N_LX result
 * on this specific file.
 *
 * Build:
 *   bcc -ms -I$(INCLUDE) -L$(LIB) dactest.c
 * (see firmware/client/MAKEFILE.)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <dos.h>

/* ===================================================================
 * Section 1: Raw port I/O and wire protocol handshakes
 *
 * Same idiom as client/sdtest.c and client/iftest.c's equivalent
 * section -- see those files' header comments for the doc-section
 * references these implement. Duplicated rather than shared for the
 * same reason as always in this project: no shared header between
 * these DOS-side translation units.
 *
 * outportb()/inportb() come straight from Borland's <dos.h> -- no
 * inline asm anywhere in this file.
 * =================================================================== */

#define CTRL_STROBE_SELECT_LOW   0x01u
#define CTRL_STROBE_SELECT_HIGH  0x00u

#define CTRL_AUTOLF_IDLE         0x02u
#define CTRL_AUTOLF_ASSERT       0x00u

#define CTRL_INIT_RAISED         0x04u
#define CTRL_INIT_DROPPED        0x00u

#define CTRL_SELECT_WRITE        0x00u
#define CTRL_SELECT_READ         0x08u

#define CTRL_IDLE_DEFAULT  (CTRL_AUTOLF_IDLE | CTRL_INIT_DROPPED | CTRL_SELECT_WRITE)

#define REG_DATA     0
#define REG_STATUS   1
#define REG_CONTROL  2

#define POLL_TIMEOUT_ITERS  60000UL
#define IO_DELAY_ITERS      4

static unsigned base_port = 0x378;
static unsigned char ctrl_shadow = CTRL_IDLE_DEFAULT;

static void ctrl_out(unsigned char value) {
    ctrl_shadow = value;
    outportb(base_port + REG_CONTROL, value);
}

static void io_delay(void) {
    int i;
    for (i = 0; i < IO_DELAY_ITERS; i++) {
        inportb(0x80);
    }
}

static void reset_hold_delay(void) {
    int i;
    for (i = 0; i < 200; i++) {
        io_delay();
    }
}

static int wait_error_high(void) {
    unsigned long i;
    for (i = 0; i < POLL_TIMEOUT_ITERS; i++) {
        if (inportb(base_port + REG_STATUS) & 0x08) {
            return 0;
        }
    }
    return 1;
}

static int wait_error_low(void) {
    unsigned long i;
    for (i = 0; i < POLL_TIMEOUT_ITERS; i++) {
        if (!(inportb(base_port + REG_STATUS) & 0x08)) {
            return 0;
        }
    }
    return 1;
}

static void host_select_direction(int want_write) {
    unsigned char v = (unsigned char)(ctrl_shadow & ~CTRL_SELECT_READ);
    v = (unsigned char)(v | (want_write ? CTRL_SELECT_WRITE : CTRL_SELECT_READ));
    ctrl_out(v);
    io_delay();
}

static void host_reset(void) {
    ctrl_out((unsigned char)(CTRL_AUTOLF_ASSERT | CTRL_INIT_DROPPED | CTRL_SELECT_WRITE));
    reset_hold_delay();
    ctrl_out(CTRL_IDLE_DEFAULT);
    io_delay();
}

static int raw_send_byte(unsigned char byte) {
    outportb(base_port + REG_DATA, byte);

    ctrl_out((unsigned char)(ctrl_shadow | CTRL_INIT_RAISED));
    if (wait_error_high()) return 1;

    ctrl_out((unsigned char)(ctrl_shadow & ~CTRL_INIT_RAISED));
    if (wait_error_low()) return 1;

    return 0;
}

static int raw_recv_byte(unsigned char *out) {
    unsigned char status, lo, hi;

    ctrl_out((unsigned char)((ctrl_shadow & ~0x01u) | CTRL_STROBE_SELECT_LOW));
    io_delay();
    ctrl_out((unsigned char)(ctrl_shadow | CTRL_INIT_RAISED));
    if (wait_error_high()) return 1;
    status = inportb(base_port + REG_STATUS);
    lo = (unsigned char)(status & 0xF0u);
    ctrl_out((unsigned char)(ctrl_shadow & ~CTRL_INIT_RAISED));
    if (wait_error_low()) return 1;

    ctrl_out((unsigned char)((ctrl_shadow & ~0x01u) | CTRL_STROBE_SELECT_HIGH));
    io_delay();
    ctrl_out((unsigned char)(ctrl_shadow | CTRL_INIT_RAISED));
    if (wait_error_high()) return 1;
    status = inportb(base_port + REG_STATUS);
    hi = (unsigned char)(status & 0xF0u);
    ctrl_out((unsigned char)(ctrl_shadow & ~CTRL_INIT_RAISED));
    if (wait_error_low()) return 1;

    *out = (unsigned char)((lo >> 4) | hi);
    return 0;
}

static int raw_recv_bulk(unsigned char *buf, unsigned count) {
    unsigned char status, lo, hi, val;
    unsigned i;
    for (i = 0; i < count; i++) {
        ctrl_out((unsigned char)((ctrl_shadow & ~0x01u) | CTRL_STROBE_SELECT_LOW));
        io_delay();
        ctrl_out((unsigned char)(ctrl_shadow | CTRL_INIT_RAISED));
        if (wait_error_high()) return 1;
        status = inportb(base_port + REG_STATUS);
        lo = (unsigned char)(status & 0xF0u);
        ctrl_out((unsigned char)(ctrl_shadow & ~CTRL_INIT_RAISED));
        if (wait_error_low()) return 1;

        ctrl_out((unsigned char)((ctrl_shadow & ~0x01u) | CTRL_STROBE_SELECT_HIGH));
        io_delay();
        ctrl_out((unsigned char)(ctrl_shadow | CTRL_INIT_RAISED));
        if (wait_error_high()) return 1;
        status = inportb(base_port + REG_STATUS);
        hi = (unsigned char)(status & 0xF0u);
        ctrl_out((unsigned char)(ctrl_shadow & ~CTRL_INIT_RAISED));
        if (wait_error_low()) return 1;

        val = (unsigned char)((lo >> 4) | hi);
        if (buf != NULL) buf[i] = val;
    }
    return 0;
}

/* ===================================================================
 * Section 2: DISPATCH_CMD_DAC Sub-protocol
 * =================================================================== */

#define DISPATCH_CMD_DAC      0x04u

#define DAC_OP_CONFIG         0x01u
#define DAC_OP_START          0x02u
#define DAC_OP_STOP           0x03u
#define DAC_OP_STATUS         0x04u
#define DAC_OP_WRITE          0x05u
#define DAC_OP_TONE           0x06u
#define DAC_OP_RESET_METRICS  0x07u

#define DAC_SCLK_INTERNAL     0x00u
#define DAC_SCLK_EXTERNAL     0x01u

#define DAC_STATUS_OK         0x00u
#define DAC_STATUS_ERROR      0x01u

typedef struct {
    unsigned char  status;
    unsigned char  state;
    unsigned long  sample_rate;
    unsigned char  channels;
    unsigned char  bits;
    unsigned char  sclk_mode;
    unsigned       ring_free;
    unsigned       ring_queued;
    unsigned long  underruns;
    unsigned long  frames_played;
} dac_telemetry_t;

/* Fetches the 21-byte DAC status packet.
 * Protocol: Send DISPATCH_CMD_DAC (Write), switch direction to Read, receive 21 bytes. */
static int dac_get_status(dac_telemetry_t *t) {
    unsigned char buf[21];
    int i;

    host_select_direction(1);
    if (raw_send_byte(DISPATCH_CMD_DAC)) return 1;
    host_select_direction(0);

    for (i = 0; i < 21; i++) {
        if (raw_recv_byte(&buf[i])) return 1;
    }

    if (t != NULL) {
        t->status = buf[0];
        t->state = buf[1];
        t->sample_rate = (unsigned long)buf[2] | ((unsigned long)buf[3] << 8) |
                         ((unsigned long)buf[4] << 16) | ((unsigned long)buf[5] << 24);
        t->channels = buf[6];
        t->bits = buf[7];
        t->sclk_mode = buf[8];
        t->ring_free = (unsigned)buf[9] | ((unsigned)buf[10] << 8);
        t->ring_queued = (unsigned)buf[11] | ((unsigned)buf[12] << 8);
        t->underruns = (unsigned long)buf[13] | ((unsigned long)buf[14] << 8) |
                       ((unsigned long)buf[15] << 16) | ((unsigned long)buf[16] << 24);
        t->frames_played = (unsigned long)buf[17] | ((unsigned long)buf[18] << 8) |
                           ((unsigned long)buf[19] << 16) | ((unsigned long)buf[20] << 24);
    }
    return 0;
}

static int dac_config(unsigned long rate, unsigned char channels, unsigned char bits, unsigned char sclk_mode) {
    host_select_direction(1);
    if (raw_send_byte(DISPATCH_CMD_DAC)) return 1;
    if (raw_send_byte(DAC_OP_CONFIG)) return 1;
    if (raw_send_byte((unsigned char)(rate & 0xFFu))) return 1;
    if (raw_send_byte((unsigned char)((rate >> 8) & 0xFFu))) return 1;
    if (raw_send_byte((unsigned char)((rate >> 16) & 0xFFu))) return 1;
    if (raw_send_byte((unsigned char)((rate >> 24) & 0xFFu))) return 1;
    if (raw_send_byte(channels)) return 1;
    if (raw_send_byte(bits)) return 1;
    if (raw_send_byte(sclk_mode)) return 1;
    return 0;
}

static int dac_start(void) {
    host_select_direction(1);
    if (raw_send_byte(DISPATCH_CMD_DAC)) return 1;
    if (raw_send_byte(DAC_OP_START)) return 1;
    return 0;
}

static int dac_stop(void) {
    host_select_direction(1);
    if (raw_send_byte(DISPATCH_CMD_DAC)) return 1;
    if (raw_send_byte(DAC_OP_STOP)) return 1;
    return 0;
}

static int dac_play_tone(unsigned freq_hz, unsigned duration_ms, unsigned char sclk_mode, unsigned long sample_rate) {
    host_select_direction(1);
    if (raw_send_byte(DISPATCH_CMD_DAC)) return 1;
    if (raw_send_byte(DAC_OP_TONE)) return 1;
    if (raw_send_byte((unsigned char)(freq_hz & 0xFFu))) return 1;
    if (raw_send_byte((unsigned char)((freq_hz >> 8) & 0xFFu))) return 1;
    if (raw_send_byte((unsigned char)(duration_ms & 0xFFu))) return 1;
    if (raw_send_byte((unsigned char)((duration_ms >> 8) & 0xFFu))) return 1;
    if (raw_send_byte(sclk_mode)) return 1;
    if (raw_send_byte((unsigned char)(sample_rate & 0xFFu))) return 1;
    if (raw_send_byte((unsigned char)((sample_rate >> 8) & 0xFFu))) return 1;
    if (raw_send_byte((unsigned char)((sample_rate >> 16) & 0xFFu))) return 1;
    if (raw_send_byte((unsigned char)((sample_rate >> 24) & 0xFFu))) return 1;
    return 0;
}

static int dac_write_chunk(const unsigned char *data, unsigned len) {
    unsigned i;

    host_select_direction(1);
    if (raw_send_byte(DISPATCH_CMD_DAC)) return 1;
    if (raw_send_byte(DAC_OP_WRITE)) return 1;
    if (raw_send_byte((unsigned char)(len & 0xFFu))) return 1;
    if (raw_send_byte((unsigned char)((len >> 8) & 0xFFu))) return 1;

    for (i = 0; i < len; i++) {
        if (raw_send_byte(data[i])) return 1;
    }
    return 0;
}

static int dac_reset_metrics(void) {
    host_select_direction(1);
    if (raw_send_byte(DISPATCH_CMD_DAC)) return 1;
    if (raw_send_byte(DAC_OP_RESET_METRICS)) return 1;
    return 0;
}

/* ===================================================================
 * Section 3: DISPATCH_CMD_SD Sub-protocol (for coexistence test)
 * =================================================================== */

#define DISPATCH_CMD_SD      0x03u
#define SD_OP_INIT           0x01u
#define SD_OP_READ_SECTORS   0x02u
#define SD_OP_CHECK_MEDIA    0x04u
#define SD_STATUS_OK         0x00u

static int sd2_get_result(unsigned char *out_status, unsigned char *buf, unsigned bufcap) {
    unsigned char hdr[5];
    unsigned len;
    int i;

    host_select_direction(1);
    if (raw_send_byte(DISPATCH_CMD_SD)) return 1;
    host_select_direction(0);

    for (i = 0; i < 5; i++) {
        if (raw_recv_byte(hdr + i)) return 1;
    }
    *out_status = hdr[0];
    len = (unsigned)hdr[3] | ((unsigned)hdr[4] << 8);

    if (buf == NULL || bufcap == 0) {
        if (raw_recv_bulk(NULL, len)) return 1;
    } else if (bufcap >= len) {
        if (raw_recv_bulk(buf, len)) return 1;
    } else {
        if (raw_recv_bulk(buf, bufcap)) return 1;
        if (raw_recv_bulk(NULL, len - bufcap)) return 1;
    }
    return 0;
}

static int sd2_init(void) {
    unsigned char status;
    host_select_direction(1);
    if (raw_send_byte(DISPATCH_CMD_SD)) return 1;
    if (raw_send_byte(SD_OP_INIT)) return 1;
    if (sd2_get_result(&status, NULL, 0)) return 1;
    return (status == SD_STATUS_OK) ? 0 : -1;
}

static int sd2_check_media(void) {
    unsigned char status;
    host_select_direction(1);
    if (raw_send_byte(DISPATCH_CMD_SD)) return 1;
    if (raw_send_byte(SD_OP_CHECK_MEDIA)) return 1;
    if (sd2_get_result(&status, NULL, 0)) return 1;
    return (status == SD_STATUS_OK) ? 0 : -1;
}

static int sd2_read_sectors(unsigned long lba, unsigned count, unsigned char *buf, unsigned bufcap) {
    unsigned char status;
    host_select_direction(1);
    if (raw_send_byte(DISPATCH_CMD_SD)) return 1;
    if (raw_send_byte(SD_OP_READ_SECTORS)) return 1;
    if (raw_send_byte((unsigned char)(lba & 0xFFu))) return 1;
    if (raw_send_byte((unsigned char)((lba >> 8) & 0xFFu))) return 1;
    if (raw_send_byte((unsigned char)((lba >> 16) & 0xFFu))) return 1;
    if (raw_send_byte((unsigned char)((lba >> 24) & 0xFFu))) return 1;
    if (raw_send_byte((unsigned char)count)) return 1;

    if (sd2_get_result(&status, buf, bufcap)) return 1;
    return (status == SD_STATUS_OK) ? 0 : -1;
}

/* ===================================================================
 * Section 4: High-Level Command Implementations
 * =================================================================== */

static unsigned char g_stream_buf[2048];
static unsigned char g_sd_sector_buf[512];

static int cmd_beep(int ext_clock) {
    dac_telemetry_t t;

    printf("dactest: Playing 1000Hz test tone (2000ms) with %s clock...\n",
           ext_clock ? "GPIO26 external MCLK" : "internal PLL (GPIO26=0)");

    if (dac_play_tone(1000, 2000, ext_clock ? DAC_SCLK_EXTERNAL : DAC_SCLK_INTERNAL, 44100ul)) {
        fprintf(stderr, "dactest: Error sending TONE command over LPT\n");
        return 1;
    }
    printf("dactest: Tone command accepted.\n");

    /* The TONE command itself already succeeded above -- the firmware's
     * tone generator is asynchronous and starts immediately, so audio
     * can be playing even if this follow-up status read has trouble.
     * Treat status as diagnostic-only here: report it if it works, but
     * don't turn a failed status read into an overall command failure. */
    if (dac_get_status(&t) == 0) {
        printf("dactest: State=%s, Rate=%lu Hz, SCLK=%s\n",
               t.state ? "PLAYING" : "IDLE", t.sample_rate,
               t.sclk_mode ? "EXTERNAL" : "INTERNAL");
    } else {
        printf("dactest: Status read failed; tone command was already accepted.\n");
    }
    return 0;
}

static int cmd_tone(unsigned freq, unsigned duration, int ext_clock, unsigned long rate) {
    dac_telemetry_t t;

    printf("dactest: Tone freq=%u Hz, duration=%u ms, clock=%s, rate=%lu Hz\n",
           freq, duration, ext_clock ? "EXTERNAL" : "INTERNAL", rate);

    if (dac_play_tone(freq, duration, ext_clock ? DAC_SCLK_EXTERNAL : DAC_SCLK_INTERNAL, rate)) {
        fprintf(stderr, "dactest: Error sending TONE command\n");
        return 1;
    }
    printf("dactest: Tone command accepted.\n");

    /* Same reasoning as cmd_beep(): status is diagnostic-only here. */
    if (dac_get_status(&t) == 0) {
        printf("dactest: State=%s, Rate=%lu Hz, SCLK=%s\n",
               t.state ? "PLAYING" : "IDLE", t.sample_rate,
               t.sclk_mode ? "EXTERNAL" : "INTERNAL");
    } else {
        printf("dactest: Status read failed; tone command was already accepted.\n");
    }
    return 0;
}

static int cmd_status(void) {
    dac_telemetry_t t;
    if (dac_get_status(&t)) {
        fprintf(stderr, "dactest: Failed to read DAC status\n");
        return 1;
    }

    printf("=== Trinket DAC Telemetry ===\n");
    printf("  Status:          %s (0x%02X)\n", (t.status == DAC_STATUS_OK) ? "OK" : "ERROR", t.status);
    printf("  Playback State:  %s\n", t.state ? "PLAYING" : "IDLE");
    printf("  Sample Rate:     %lu Hz\n", t.sample_rate);
    printf("  Channels:        %u (%s)\n", t.channels, (t.channels == 1) ? "Mono" : "Stereo");
    printf("  Bits per sample: %u-bit\n", t.bits);
    printf("  SCLK Mode:       %s (0x%02X)\n", t.sclk_mode ? "External PWM on GPIO26" : "Internal PLL (GPIO26 Low)", t.sclk_mode);
    printf("  Ring Free Space: %u bytes\n", t.ring_free);
    printf("  Ring Queued:     %u bytes\n", t.ring_queued);
    printf("  Total Underruns: %lu\n", t.underruns);
    printf("  Frames Played:   %lu\n", t.frames_played);
    return 0;
}

static int cmd_stop(void) {
    printf("dactest: Stopping DAC playback...\n");
    if (dac_stop()) {
        fprintf(stderr, "dactest: Failed to send STOP command\n");
        return 1;
    }
    printf("dactest: DAC stopped.\n");
    return 0;
}

static int cmd_play(const char *filename, unsigned long rate, unsigned char channels, int ext_clock, unsigned char bits) {
    FILE *fp = fopen(filename, "rb");
    size_t nread;
    unsigned long total_bytes;
    int i;
    dac_telemetry_t final_stat;

    if (!fp) {
        fprintf(stderr, "dactest: Cannot open PCM file '%s'\n", filename);
        return 2;
    }

    printf("dactest: Streaming '%s' (%lu Hz, %u-bit, %s, %s clock)...\n",
           filename, rate, (unsigned)bits, (channels == 1) ? "Mono" : "Stereo",
           ext_clock ? "EXTERNAL" : "INTERNAL");

    /* File is streamed byte-for-byte either way -- an 8-bit PCM file
     * just has half as many bytes per sample already, so nothing below
     * this call needs to know or care which width it's pushing. The
     * trinket firmware does the actual 8->16 bit upsampling on
     * ingestion (subsys_dac.c's handle_op_write()/upsample_8_to_16())
     * once this `bits` value reaches it via DAC_OP_CONFIG -- see
     * subsys_dac.h's protocol comment. */
    if (dac_config(rate, channels, bits, ext_clock ? DAC_SCLK_EXTERNAL : DAC_SCLK_INTERNAL)) {
        fprintf(stderr, "dactest: Failed to configure DAC\n");
        fclose(fp);
        return 1;
    }

    if (dac_reset_metrics()) {
        fprintf(stderr, "dactest: Failed to reset metrics\n");
        fclose(fp);
        return 1;
    }

    // Pre-buffer initial audio data before starting playback. The return
    // value matters here: if this silently fails, dac_start() below would
    // begin playback on an empty/partial buffer while reporting success.
    nread = fread(g_stream_buf, 1, sizeof(g_stream_buf), fp);
    if (nread > 0) {
        if (dac_write_chunk(g_stream_buf, (unsigned)nread)) {
            fprintf(stderr, "dactest: Failed to pre-buffer audio\n");
            fclose(fp);
            return 1;
        }
    }

    if (dac_start()) {
        fprintf(stderr, "dactest: Failed to start DAC\n");
        fclose(fp);
        return 1;
    }

    // Streaming loop: no per-chunk status poll here on purpose. The
    // firmware's WRITE handler now applies real backpressure (blocks
    // until ring space frees up rather than silently dropping PCM), so
    // this loop doesn't need to guess buffer state via a diagnostic
    // status transaction that shouldn't be load-bearing for playback --
    // dac_write_chunk() itself will simply take longer to return when
    // the ring is full.
    total_bytes = nread;
    while ((nread = fread(g_stream_buf, 1, sizeof(g_stream_buf), fp)) > 0) {
        if (dac_write_chunk(g_stream_buf, (unsigned)nread)) {
            fprintf(stderr, "dactest: Error streaming chunk\n");
            break;
        }
        total_bytes += nread;
    }

    fclose(fp);
    printf("dactest: Reached EOF (streamed %lu bytes). Waiting for buffer to drain...\n", total_bytes);

    // Wait for buffer to drain
    for (i = 0; i < 50; i++) {
        dac_telemetry_t t;
        if (dac_get_status(&t) == 0) {
            if (t.ring_queued < 512) break;
        }
        reset_hold_delay();
    }

    dac_stop();

    if (dac_get_status(&final_stat) == 0) {
        printf("dactest: Complete! Underruns=%lu, Frames played=%lu\n",
               final_stat.underruns, final_stat.frames_played);
    }
    return 0;
}

/* ===================================================================
 * Section 5: Coexistence Test Jig (Bring-up Goals 1 & 2)
 * =================================================================== */

static int cmd_coex(unsigned sectors_batch, unsigned iterations) {
    dac_telemetry_t t0;
    unsigned sd_success_count = 0;
    unsigned sd_fail_count = 0;
    unsigned iter;
    dac_telemetry_t t_final;

    printf("============================================================\n");
    printf(" Trinket SD + DAC Coexistence & Multiplexing Test Jig\n");
    printf("============================================================\n");

    if (sectors_batch == 0) sectors_batch = 1;
    if (iterations == 0) iterations = 20;

    // 1. Initialize SD card
    printf("[1/4] Probing SD card via DISPATCH_CMD_SD...\n");
    if (sd2_init() != 0) {
        fprintf(stderr, "dactest: WARNING - SD card init failed or not present. Probing media check...\n");
    } else {
        printf("      SD card initialized successfully.\n");
    }

    // 2. Start a 10-second background tone on the DAC
    printf("[2/4] Starting 10-second test tone (440Hz A4, 44.1kHz) on DAC...\n");
    if (dac_reset_metrics() != 0) return 1;
    if (dac_play_tone(440, 10000, DAC_SCLK_INTERNAL, 44100ul) != 0) {
        fprintf(stderr, "dactest: Failed to start background tone on DAC\n");
        return 1;
    }

    if (dac_get_status(&t0) != 0 || t0.state != 1) {
        fprintf(stderr, "dactest: DAC failed to enter PLAYING state\n");
        return 1;
    }
    printf("      DAC is actively playing audio in background (underruns=%lu).\n", t0.underruns);

    // 3. Execute interleaved SD storage transactions over LPT while audio is playing
    printf("[3/4] Executing %u interleaved SD read iterations (%u sectors/batch)...\n",
           iterations, sectors_batch);

    for (iter = 0; iter < iterations; iter++) {
        int mc;
        int rd;

        // A. Perform media check probe
        mc = sd2_check_media();
        (void)mc;

        // B. Perform sector read
        rd = sd2_read_sectors(iter % 100, sectors_batch, g_sd_sector_buf, sizeof(g_sd_sector_buf));
        if (rd == 0) {
            sd_success_count++;
        } else {
            sd_fail_count++;
        }

        // C. Interleave with a DAC status poll
        if ((iter % 5) == 0) {
            dac_telemetry_t cur_t;
            if (dac_get_status(&cur_t) == 0) {
                printf("      [Iter %2u] SD Read: %s | DAC State: %s | Underruns: %lu | Frames: %lu\n",
                       iter, (rd == 0) ? "OK" : "ERR",
                       cur_t.state ? "PLAYING" : "IDLE",
                       cur_t.underruns, cur_t.frames_played);
            }
        }
    }

    // 4. Query final results
    printf("[4/4] Evaluating multiplexing results...\n");
    dac_get_status(&t_final);
    dac_stop();

    printf("\n=== Coexistence Summary ===\n");
    printf("  SD Transactions Attempted: %u\n", iterations);
    printf("  SD Transactions Succeeded: %u\n", sd_success_count);
    printf("  SD Transactions Failed:    %u\n", sd_fail_count);
    printf("  DAC Audio Underruns:       %lu\n", t_final.underruns);
    printf("  DAC Audio Frames Played:   %lu\n", t_final.frames_played);

    if (t_final.underruns == 0 && sd_fail_count == 0) {
        printf("\n>>> RESULT: SUCCESS! Storage and Audio coexisted with ZERO interference! <<<\n\n");
        return 0;
    } else if (t_final.underruns == 0) {
        printf("\n>>> RESULT: DAC audio remained solid (0 underruns), check SD card insertion. <<<\n\n");
        return 0;
    } else {
        printf("\n>>> RESULT: Experienced %lu DAC underruns during SD traffic. <<<\n\n", t_final.underruns);
        return 1;
    }
}

/* ===================================================================
 * Main Entry Point
 * =================================================================== */

static unsigned long parse_hex_ul(const char *s) {
    unsigned long v = 0;
    sscanf(s, "%lx", &v);
    return v;
}

int main(int argc, char **argv) {
    const char *cmd = NULL;
    const char *args[5];
    int nargs = 0;
    int i;

    base_port = 0x378;

    for (i = 1; i < argc; i++) {
        if ((argv[i][0] == '/' || argv[i][0] == '-') &&
            (argv[i][1] == 'P' || argv[i][1] == 'p') && argv[i][2] == '=') {
            base_port = (unsigned)parse_hex_ul(argv[i] + 3);
        } else if (cmd == NULL) {
            cmd = argv[i];
        } else if (nargs < 5) {
            args[nargs++] = argv[i];
        }
    }

    if (cmd == NULL) {
        fprintf(stderr,
            "Trinket PCM5102 DAC Test Utility\n"
            "Usage: DACTEST <command> [args] [/P=xxxx]\n\n"
            "Commands:\n"
            "  beep                          Play 1000Hz test tone (2000ms) with internal PLL (GPIO26=0)\n"
            "  beep_ext                      Play 1000Hz test tone (2000ms) with GPIO26 external clock\n"
            "  tone <freq> <ms> [int|ext] [rate]  Play custom test tone\n"
            "  status                        Query DAC state, buffer headroom, and underruns\n"
            "  stop                          Stop DAC playback\n"
            "  play <file.pcm> [rate] [1|2] [int|ext] [8|16]  Stream raw PCM file to DAC (default 16-bit)\n"
            "  coex [sectors] [iterations]   Test concurrent SD storage + DAC audio coexistence\n");
        return 3;
    }

    ctrl_out(CTRL_IDLE_DEFAULT);
    host_reset();

    if (strcmp(cmd, "beep") == 0) {
        return cmd_beep(0);
    } else if (strcmp(cmd, "beep_ext") == 0) {
        return cmd_beep(1);
    } else if (strcmp(cmd, "tone") == 0) {
        unsigned freq = (nargs >= 1) ? (unsigned)atoi(args[0]) : 1000u;
        unsigned ms   = (nargs >= 2) ? (unsigned)atoi(args[1]) : 2000u;
        int ext       = (nargs >= 3 && strcmp(args[2], "ext") == 0);
        unsigned long rate = (nargs >= 4) ? strtoul(args[3], NULL, 10) : 44100ul;
        return cmd_tone(freq, ms, ext, rate);
    } else if (strcmp(cmd, "status") == 0) {
        return cmd_status();
    } else if (strcmp(cmd, "stop") == 0) {
        return cmd_stop();
    } else if (strcmp(cmd, "play") == 0 && nargs >= 1) {
        unsigned long rate = (nargs >= 2) ? strtoul(args[1], NULL, 10) : 44100ul;
        unsigned char ch   = (nargs >= 3) ? (unsigned char)atoi(args[2]) : 2;
        int ext            = (nargs >= 4 && strcmp(args[3], "ext") == 0);
        unsigned char bits = (nargs >= 5) ? (unsigned char)atoi(args[4]) : 16;
        if (bits != 8 && bits != 16) {
            fprintf(stderr, "dactest: bit depth must be 8 or 16 (got '%s')\n", args[4]);
            return 3;
        }
        return cmd_play(args[0], rate, ch, ext, bits);
    } else if (strcmp(cmd, "coex") == 0) {
        unsigned sectors = (nargs >= 1) ? (unsigned)atoi(args[0]) : 1u;
        unsigned iters   = (nargs >= 2) ? (unsigned)atoi(args[1]) : 20u;
        return cmd_coex(sectors, iters);
    } else {
        fprintf(stderr, "dactest: Unknown command '%s'. Run without arguments for help.\n", cmd);
        return 3;
    }
}
