/*
 * dsstest.c -- Disney Sound Source emulation test client for DOS,
 *              Borland C++ 3.1, small model.
 *
 * Behaves like a game that already knows how to drive a real Disney
 * Sound Source on the parallel port: it never speaks the trinket's own
 * dispatch protocol. It writes the control register as 04h / 0Ch, puts
 * a byte on D0-D7, pulses pin 17 low then high (the rising edge clocks
 * the byte into the FIFO), and polls status bit 6 (nAck, pin 10) for
 * "FIFO full" -- the same sequence as the reference routine in the
 * DSS Programmer's Guide and the one Wolfenstein 3D's SDL_SSService()
 * uses. If the trinket's DSS mode (firmware/src/subsys_dss.c) works,
 * this tool should pass and make sound with NO trinket-specific code
 * on the host.
 *
 * Commands (all take an optional /P=378 port base, default 378):
 *
 *   DSSTEST detect
 *     Presence/behaviour check, modelled on how games detect a DSS:
 *     power up, shove 32 bytes in back to back and expect the FIFO-full
 *     flag, wait and expect it to clear, then count how many writes it
 *     takes to fill (expect 15: the trinket raises nAck one byte short
 *     of the 16-byte FIFO on purpose). Exit code 0 = all checks passed.
 *
 *   DSSTEST rate [seconds]
 *     Streams silence as fast as the FIFO allows (status-polled, like a
 *     game's ISR) and times it against the BIOS tick: this is the
 *     drain clock as the HOST sees it through the nAck line. Expect
 *     ~7000 Hz (BIOS tick resolution limits it to a few percent).
 *
 *   DSSTEST tone [freq_hz] [seconds]
 *     Status-paced sine wave at the DSS's 7000 Hz sample rate. A wrong
 *     pitch means the drain rate is wrong. Defaults: 440 Hz, 3 s.
 *
 *   DSSTEST play <file.raw> [loops] [/B=<sectors>]
 *     Streams unsigned 8-bit mono raw audio (7000 Hz is what the
 *     hardware plays at -- a 16-bit or 44.1 kHz file will sound like
 *     noise) through the same paced loop.
 *
 *     The file is loaded into RAM BEFORE playback starts, and playback
 *     never touches the disk. That is deliberate: a real DSS has a
 *     16-byte FIFO (2.3 ms), so any foreground disk read in the middle
 *     of a stream starves it, and a read through the trinket's own
 *     driver is much longer than that -- it also takes the port away
 *     from the Sound Source while it runs. (A game gets away with disk
 *     access mid-sound because its timer ISR keeps feeding the FIFO;
 *     this tool is a foreground loop with no ISR.) A file bigger than
 *     the free far heap is played in segments, and each segment load
 *     is an audible gap. /B=<sectors> caps the segment size, in
 *     512-byte sectors (so /B=1 is 512 bytes, /B=2 is 1 KB).
 *
 * Any key (or Ctrl-Break) stops a running stream cleanly. On exit the
 * control register is put back to the trinket's idle value (02h) so the
 * dispatcher takes over again -- a real game leaves pin 17 low and
 * doesn't care, but TRINKET.SYS and DACTEST do.
 *
 * After a run, `DACTEST status` shows the DAC's underrun count and ring
 * state (the firmware's `ESC` console also prints the DSS counters).
 *
 * C89 only (declarations at the top of blocks, no for-scoped
 * variables), same constraints as dactest.c -- see its header comment.
 *
 * Build: bcc -ms -I$(INCLUDE) -L$(LIB) dsstest.c   (firmware/client/MAKEFILE)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dos.h>
#include <conio.h>
#include <bios.h>
#include <alloc.h>
#include <mem.h>

#define REG_DATA     0
#define REG_STATUS   1
#define REG_CONTROL  2

/* DSS Programmer's Guide control values (bit 1 clear = pin 14 high,
 * bit 2 set = pin 16 high; bit 3 = pin 17, inverted by the port). */
#define DSS_CTRL_ON       0x04u    /* power-up / strobe released (pin 17 high) */
#define DSS_CTRL_STROBE   0x0Cu    /* strobe low / power-down    (pin 17 low)  */

/* The trinket's own idle control value (dactest.c: CTRL_IDLE_DEFAULT). */
#define TRINKET_CTRL_IDLE 0x02u

#define STATUS_FULL       0x40u    /* bit 6 = nAck: high while FIFO is full */

#define DSS_RATE          7000UL
#define STUCK_POLLS       200000UL /* polls before declaring nAck stuck high */

static unsigned base_port = 0x378;
static volatile int g_abort = 0;

static int ctrlbrk_handler(void) {
    g_abort = 1;
    return 1;               /* non-zero: keep running, let the loops notice */
}

static void io_pause(void) {
    /* One ISA I/O cycle (~1-2 us on an XT-class bus); stands in for the
     * push/pop pair the reference routine uses to stretch the strobe. */
    inportb(0x80);
}

static void ctrl(unsigned char v) {
    outportb(base_port + REG_CONTROL, v);
}

static int fifo_full(void) {
    return (inportb(base_port + REG_STATUS) & STATUS_FULL) != 0;
}

/* One strobe, exactly as the guide's routine: data, pin 17 low, short
 * hold, pin 17 high (rising edge latches), short settle. */
static void strobe_byte(unsigned char v) {
    outportb(base_port + REG_DATA, v);
    ctrl(DSS_CTRL_STROBE);
    io_pause();
    io_pause();
    ctrl(DSS_CTRL_ON);
    io_pause();
}

/* Status-paced write, like a game's timer ISR: wait for "not full",
 * then send. Returns 0 on success, 1 if nAck never dropped. */
static int paced_write(unsigned char v) {
    unsigned long polls = 0;
    while (fifo_full()) {
        if (++polls > STUCK_POLLS) {
            return 1;
        }
    }
    strobe_byte(v);
    return 0;
}

static void dss_power_up(void) {
    ctrl(DSS_CTRL_ON);
    delay(5);
}

static void dss_finish(void) {
    /* A real game leaves pin 17 low (unit off); then hand the port back
     * to the trinket dispatcher. */
    ctrl(DSS_CTRL_STROBE);
    delay(2);
    ctrl(TRINKET_CTRL_IDLE);
    delay(2);
}

static int stop_requested(void) {
    if (g_abort) {
        return 1;
    }
    if (kbhit()) {
        getch();
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */

static int cmd_detect(void) {
    int pass = 1;
    int i;
    int depth;
    int before, after32, after_wait;

    printf("DSS emulation check on port %03Xh\n", base_port);

    dss_power_up();
    before = fifo_full();
    printf("  power-up (ctrl=04h): nAck = %d   [%s]\n", before,
           before ? "FAIL: FIFO reports full with nothing written" : "ok");
    if (before) pass = 0;

    for (i = 0; i < 32; i++) {
        outportb(base_port + REG_DATA, 0x80);
        ctrl(DSS_CTRL_STROBE);
        io_pause();
        ctrl(DSS_CTRL_ON);
    }
    after32 = fifo_full();
    printf("  32 back-to-back bytes: nAck = %d   [%s]\n", after32,
           after32 ? "ok: FIFO overflowed, so it exists" : "FAIL: never saw FIFO-full (no DSS here)");
    if (!after32) pass = 0;

    delay(10);
    after_wait = fifo_full();
    printf("  10 ms later:           nAck = %d   [%s]\n", after_wait,
           after_wait ? "FAIL: FIFO never drained" : "ok: drained");
    if (after_wait) pass = 0;

    /* How many writes does it take to fill? Status checked after each
     * strobe, no waiting, so at most a tick or so drains meanwhile. */
    depth = 0;
    while (!fifo_full() && depth < 64) {
        strobe_byte(0x80);
        depth++;
    }
    /* The trinket raises nAck one byte short of the 16-byte FIFO (a one-slot
     * guard against a late flag), so a clean run reports 15; a drain tick
     * landing mid-loop can add one or two. */
    printf("  writes until full:     %d      [%s]\n", depth,
           (depth >= 15 && depth <= 17) ? "ok (expect 15, +1 or 2 if a tick landed)"
                                         : "unexpected (expect 15)");
    if (depth < 15 || depth > 17) pass = 0;

    delay(10);
    dss_finish();
    printf("%s\n", pass ? "RESULT: PASS" : "RESULT: FAIL");
    return pass ? 0 : 1;
}

static int cmd_rate(unsigned seconds) {
    unsigned long n, i, target;
    long t0, t1, tn;
    unsigned long ticks, hz;
    int stuck = 0;

    if (seconds == 0) seconds = 2;
    target = (unsigned long)seconds * DSS_RATE;

    printf("DSS drain-rate measurement, %u s of samples on port %03Xh (any key stops)\n",
           seconds, base_port);
    dss_power_up();

    /* Start on a BIOS tick edge so only the end can be off by one. */
    t0 = biostime(0, 0L);
    do { tn = biostime(0, 0L); } while (tn == t0);
    t0 = tn;

    n = 0;
    for (i = 0; i < target; i++) {
        if ((i & 0xFFu) == 0 && stop_requested()) break;
        if (paced_write(0x80)) { stuck = 1; break; }
        n++;
    }
    t1 = biostime(0, 0L);
    dss_finish();

    if (stuck) {
        printf("FAIL: nAck stayed high -- FIFO never drained after %lu bytes\n", n);
        return 1;
    }
    ticks = (unsigned long)(t1 - t0);
    if (ticks == 0) {
        printf("run too short to time (%lu bytes)\n", n);
        return 1;
    }
    /* BIOS tick = 18.2065 Hz */
    hz = (n * 18207UL) / (ticks * 1000UL);
    printf("  %lu bytes in %lu BIOS ticks -> about %lu Hz (nominal 7000, resolution +/- %lu%%)\n",
           n, ticks, hz, 100UL / ticks + 1UL);
    printf("  now run:  DACTEST status   (underruns should be 0)\n");
    return 0;
}

static const unsigned char sine256[256] = {
    128, 130, 133, 135, 138, 140, 143, 145, 148, 150, 152, 155, 157, 159, 162, 164,
    166, 169, 171, 173, 175, 177, 179, 181, 184, 186, 188, 190, 191, 193, 195, 197,
    199, 200, 202, 204, 205, 207, 208, 210, 211, 212, 214, 215, 216, 217, 218, 219,
    220, 221, 222, 223, 224, 224, 225, 226, 226, 227, 227, 227, 228, 228, 228, 228,
    228, 228, 228, 228, 228, 227, 227, 227, 226, 226, 225, 224, 224, 223, 222, 221,
    220, 219, 218, 217, 216, 215, 214, 212, 211, 210, 208, 207, 205, 204, 202, 200,
    199, 197, 195, 193, 191, 190, 188, 186, 184, 181, 179, 177, 175, 173, 171, 169,
    166, 164, 162, 159, 157, 155, 152, 150, 148, 145, 143, 140, 138, 135, 133, 130,
    128, 126, 123, 121, 118, 116, 113, 111, 108, 106, 104, 101,  99,  97,  94,  92,
     90,  87,  85,  83,  81,  79,  77,  75,  72,  70,  68,  66,  65,  63,  61,  59,
     57,  56,  54,  52,  51,  49,  48,  46,  45,  44,  42,  41,  40,  39,  38,  37,
     36,  35,  34,  33,  32,  32,  31,  30,  30,  29,  29,  29,  28,  28,  28,  28,
     28,  28,  28,  28,  28,  29,  29,  29,  30,  30,  31,  32,  32,  33,  34,  35,
     36,  37,  38,  39,  40,  41,  42,  44,  45,  46,  48,  49,  51,  52,  54,  56,
     57,  59,  61,  63,  65,  66,  68,  70,  72,  75,  77,  79,  81,  83,  85,  87,
     90,  92,  94,  97,  99, 101, 104, 106, 108, 111, 113, 116, 118, 121, 123, 126
};

static int cmd_tone(unsigned freq, unsigned seconds) {
    unsigned long total, i;
    unsigned phase = 0;
    unsigned step;
    int stuck = 0;

    if (freq == 0) freq = 440;
    if (freq > 3000) freq = 3000;
    if (seconds == 0) seconds = 3;
    step = (unsigned)(((unsigned long)freq * 65536UL) / DSS_RATE);
    total = (unsigned long)seconds * DSS_RATE;

    printf("DSS tone: %u Hz for %u s at 7000 Hz sample rate on port %03Xh (any key stops)\n",
           freq, seconds, base_port);
    dss_power_up();
    for (i = 0; i < total; i++) {
        if ((i & 0xFFu) == 0 && stop_requested()) break;
        if (paced_write(sine256[phase >> 8])) { stuck = 1; break; }
        phase += step;
    }
    paced_write(0x80);
    dss_finish();
    if (stuck) {
        printf("FAIL: nAck stayed high -- FIFO never drained\n");
        return 1;
    }
    printf("  done. If the pitch sounded wrong, the drain rate is off (see: DSSTEST rate).\n");
    printf("  now run:  DACTEST status   (underruns should be 0)\n");
    return 0;
}

#define FAR_RESERVE  8192UL   /* leave this much of the far heap for DOS/stdio */

/* Reads up to `want` bytes starting at the file's current position into
 * far memory. Returns the number of bytes actually read. */
static unsigned long load_segment(FILE *f, unsigned char huge *dst, unsigned long want) {
    unsigned char buf[1024];
    unsigned long got = 0;
    unsigned n, k, take;

    while (got < want) {
        take = (want - got > (unsigned long)sizeof(buf)) ? (unsigned)sizeof(buf)
                                                          : (unsigned)(want - got);
        n = (unsigned)fread(buf, 1, take, f);
        if (n == 0) break;
        for (k = 0; k < n; k++) {
            *dst++ = buf[k];
        }
        got += n;
        if (((got >> 10) & 0x3Fu) == 0 && (got & 0x3FFu) < (unsigned long)n) {
            putchar('.');           /* a dot every 64 KB so a slow load doesn't look hung */
            fflush(stdout);
        }
        if (g_abort) break;
    }
    return got;
}

static int play_buffer(unsigned char huge *mem, unsigned long len, unsigned long *sent) {
    unsigned char huge *p = mem;
    unsigned long i;

    for (i = 0; i < len; i++) {
        if ((i & 0xFFu) == 0 && stop_requested()) return 2;
        if (paced_write(*p++)) return 1;
        (*sent)++;
    }
    return 0;
}

static int cmd_play(const char *path, unsigned loops, unsigned long cap_sectors) {
    FILE *f;
    unsigned char huge *mem;
    unsigned long filesize, avail, want, got, offset, sent = 0;
    unsigned pass;
    int whole, rc = 0, seg = 0;

    f = fopen(path, "rb");
    if (f == NULL) {
        fprintf(stderr, "dsstest: cannot open '%s'\n", path);
        return 3;
    }
    fseek(f, 0L, SEEK_END);
    filesize = (unsigned long)ftell(f);
    rewind(f);
    if (filesize == 0UL) {
        fprintf(stderr, "dsstest: '%s' is empty\n", path);
        fclose(f);
        return 3;
    }
    if (loops == 0) loops = 1;

    avail = farcoreleft();
    avail = (avail > FAR_RESERVE) ? (avail - FAR_RESERVE) : 0UL;
    want = filesize;
    if (cap_sectors != 0UL && want > cap_sectors * 512UL) want = cap_sectors * 512UL;
    if (want > avail) want = avail;
    if (want == 0UL) {
        fprintf(stderr, "dsstest: no free far memory to preload into\n");
        fclose(f);
        return 3;
    }
    mem = (unsigned char huge *)farmalloc(want);
    if (mem == NULL) {
        fprintf(stderr, "dsstest: farmalloc(%lu) failed\n", want);
        fclose(f);
        return 3;
    }
    whole = (want >= filesize);

    printf("DSS play: %s (%lu KB) x%u on port %03Xh, preloading ",
           path, (filesize + 1023UL) / 1024UL, loops, base_port);
    if (whole) {
        printf("all %lu KB", (want + 1023UL) / 1024UL);
    } else {
        printf("segments of %lu sectors (%lu bytes)", (want + 511UL) / 512UL, want);
    }
    printf(" (any key stops)\n");

    for (pass = 0; pass < loops && rc == 0; pass++) {
        offset = 0UL;
        while (offset < filesize && rc == 0) {
            if (!whole || pass == 0) {
                /* The SD card (or whichever drive) is read here, with the
                 * Sound Source idle -- never while audio is playing. */
                ctrl(TRINKET_CTRL_IDLE);
                fseek(f, (long)offset, SEEK_SET);
                printf("  loading %s%d ", whole ? "file" : "segment ", ++seg);
                got = load_segment(f, mem, want);
                if (whole) {
                    printf(" %lu KB\n", (got + 1023UL) / 1024UL);
                } else {
                    printf(" %lu sectors\n", (got + 511UL) / 512UL);
                }
                if (got == 0UL) { rc = 3; break; }
            } else {
                got = filesize;
            }
            dss_power_up();
            rc = play_buffer(mem, got, &sent);
            offset += got;
            if (g_abort) rc = 2;
        }
    }
    paced_write(0x80);
    dss_finish();
    farfree((void far *)mem);
    fclose(f);

    if (rc == 1) {
        printf("FAIL: nAck stayed high -- FIFO never drained after %lu bytes\n", sent);
        return 1;
    }
    printf("  sent %lu bytes (~%lu s of audio)\n", sent, sent / DSS_RATE);
    printf("  now run:  DACTEST status   (underruns should be 0 or 1)\n");
    return (rc == 3) ? 3 : 0;
}

/* ------------------------------------------------------------------ */

static unsigned long parse_hex_ul(const char *s) {
    unsigned long v = 0;
    sscanf(s, "%lx", &v);
    return v;
}

int main(int argc, char **argv) {
    const char *cmd = NULL;
    const char *args[3];
    int nargs = 0;
    int i;
    int rc;
    unsigned long cap_sectors = 0UL;

    for (i = 1; i < argc; i++) {
        if ((argv[i][0] == '/' || argv[i][0] == '-') &&
            (argv[i][1] == 'P' || argv[i][1] == 'p') && argv[i][2] == '=') {
            base_port = (unsigned)parse_hex_ul(argv[i] + 3);
        } else if ((argv[i][0] == '/' || argv[i][0] == '-') &&
                   (argv[i][1] == 'B' || argv[i][1] == 'b') && argv[i][2] == '=') {
            cap_sectors = (unsigned long)atol(argv[i] + 3);
        } else if (cmd == NULL) {
            cmd = argv[i];
        } else if (nargs < 3) {
            args[nargs++] = argv[i];
        }
    }

    if (cmd == NULL) {
        fprintf(stderr,
            "Trinket Disney Sound Source emulation test\n"
            "Usage: DSSTEST <command> [args] [/P=xxxx]\n\n"
            "Commands:\n"
            "  detect                  Presence/behaviour check (exit code 0 = pass)\n"
            "  rate [seconds]          Measure the FIFO drain rate as the host sees it\n"
            "  tone [freq] [seconds]   Status-paced sine at 7000 Hz (default 440 Hz, 3 s)\n"
            "  play <file.raw> [loops] Preload, then stream unsigned 8-bit 7000 Hz mono raw\n"
            "                          (/B=<sectors> caps the preload segment, 512 bytes each)\n");
        return 3;
    }

    ctrlbrk(ctrlbrk_handler);
    ctrl(TRINKET_CTRL_IDLE);

    if (strcmp(cmd, "detect") == 0) {
        rc = cmd_detect();
    } else if (strcmp(cmd, "rate") == 0) {
        rc = cmd_rate((nargs >= 1) ? (unsigned)atoi(args[0]) : 2u);
    } else if (strcmp(cmd, "tone") == 0) {
        rc = cmd_tone((nargs >= 1) ? (unsigned)atoi(args[0]) : 440u,
                      (nargs >= 2) ? (unsigned)atoi(args[1]) : 3u);
    } else if (strcmp(cmd, "play") == 0 && nargs >= 1) {
        rc = cmd_play(args[0], (nargs >= 2) ? (unsigned)atoi(args[1]) : 1u, cap_sectors);
    } else {
        fprintf(stderr, "dsstest: unknown command '%s'. Run without arguments for help.\n", cmd);
        return 3;
    }

    ctrl(TRINKET_CTRL_IDLE);
    return rc;
}
