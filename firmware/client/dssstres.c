/*
 * dssstres.c -- stress test for "a game's timer interrupt vs. a disk
 *                transaction on the same parallel port".
 *                DOS, Borland C++ 3.1, small model.
 *
 * Why this exists
 * ---------------
 * A game that feeds a Disney Sound Source does it from a timer interrupt
 * handler that writes the port's data and control registers as absolute
 * values at any instruction boundary (Wolfenstein 3D: TickBase*10 =
 * 700 Hz, each call refilling the FIFO with a burst of strobes). The
 * trinket's SD transactions use the SAME port. If the handler fires in
 * the middle of a transaction it can put the game's sample byte on the
 * data pins in place of the driver's byte (silent corruption) or make the
 * firmware abandon the command (a DOS error). TRINKET.SYS now guards
 * against that by running every transaction with interrupts disabled
 * (see "Wire lock" in src/TRINKET.C, /l= in the README). This tool is
 * the test that proves the guard works -- and that the problem is real
 * when it is switched off (/l=0 in CONFIG.SYS).
 *
 * What it does
 * ------------
 * It installs the same kind of timer interrupt Wolf3D does: PIT channel 0
 * reprogrammed to ~700 Hz, chained to the BIOS handler so the DOS clock
 * keeps correct time, and each tick runs the Sound Source service loop
 * (status bit 6 clear -> data byte, strobe 0Ch, 04h) with the same
 * sequence as SDL_SSService() in ID_SD.C. Meanwhile the foreground reads
 * or writes a file on the trinket drive and checks every byte.
 *
 *   DSSSTRES read <file> [passes] [/P=378]
 *     <file> is any file on the trinket drive, at least ~64 KB (bigger
 *     is better). A baseline pass reads it with no interrupt feeder
 *     running and records its CRC-32 and length; then `passes` more
 *     passes (default 20) read it with the feeder running, cycling the
 *     read size, and each must match the baseline. Any mismatch, short
 *     read or DOS error is a failure.
 *
 *   DSSSTRES write <file> [kilobytes] [/P=378]
 *     Creates <file> (default 128 KB) on the trinket drive with the
 *     feeder running, then stops the feeder, reads it back and compares
 *     against the pseudo-random pattern it wrote, then DELETES it. This
 *     is the dangerous direction -- a collision on a write corrupts the
 *     sector silently -- so it writes to the SD card: use a scratch
 *     card or a throwaway file name, never an existing file.
 *
 *   /INJECT   Self-test of the checker: deliberately corrupts one byte
 *             of one pass so you can see a failure reported (and the
 *             exit code go non-zero).
 *   /SLIP=<s> Self-test of the clock resync: knocks <s> seconds off the
 *             DOS clock at the end of the run, then the normal resync
 *             should report about that many seconds and put it right.
 *
 * Exit code: 0 = every pass clean, 1 = failure(s) detected, 3 = usage or
 * could not run. DOS critical errors (Abort/Retry/Ignore) are caught and
 * counted instead of prompting.
 *
 * Reading the result
 *   - With TRINKET.SYS at its default /l=1 this should PASS.
 *   - With /l=0 it should FAIL on a machine fast enough to hit the
 *     window; the point is that failures appear when the guard is off.
 *   - "timer interrupts serviced" vs "expected" (needs an RTC) shows how
 *     many timer ticks the interrupts-off windows cost. The DOS clock
 *     slips by the same amount during a run; with an RTC the tool puts
 *     it back on exit and says how far it had slipped.
 *   - While it runs, ESC on the firmware's USB console prints the DSS
 *     session counters (re-entries, strobes captured, drops).
 *   - A tone is played throughout, so you can also hear the gaps each
 *     disk chunk makes.
 *
 * Any key or Ctrl-Break stops it cleanly (PIT, vector and port restored).
 *
 * C89 only, same constraints as dactest.c / dsstest.c.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dos.h>
#include <conio.h>
#include <io.h>
#include <bios.h>
#include <fcntl.h>
#include <sys/stat.h>

#define REG_DATA     0
#define REG_STATUS   1
#define REG_CONTROL  2

/* Same values as ID_SD.C (ssOn / ssOff) and dsstest.c. */
#define DSS_CTRL_ON       0x04u
#define DSS_CTRL_STROBE   0x0Cu
#define TRINKET_CTRL_IDLE 0x02u
#define STATUS_FULL       0x40u

/* PIT divisor for ~700 Hz: 1193182 / 1705 = 699.8 Hz. Wolf3D uses the
 * same rate (TickBase*10) while a digitized sound is playing. */
#define PIT_DIV           1705u
#define PIT_HZ_X10        6998UL           /* 699.8 Hz, tenths */

#define TONE_LEN          896              /* 64 periods of the 14-sample sine */
#define MAX_BURST         32               /* hard cap per interrupt */

static unsigned base_port = 0x378;

static volatile int           ss_on = 0;
static volatile unsigned long isr_count = 0;
static volatile unsigned long strobes = 0;
static volatile unsigned long capped = 0;  /* interrupts that hit MAX_BURST */
static unsigned               acc = 0;
static unsigned               tpos = 0;
static unsigned char          tone[TONE_LEN];

static void interrupt (*old8)(void);
static int installed = 0;

static volatile int g_abort = 0;
static volatile unsigned crit_errors = 0;

/* Set when the feeder is removed: 1 = DOS clock resynced from the RTC
 * (g_clock_behind says by how much it had slipped), 2 = no RTC to resync
 * from, 0 = feeder never ran. */
static int  g_clock_state = 0;
static long g_clock_behind = 0;
static unsigned slip_secs = 0;      /* /SLIP=<s>: self-test of the clock resync */

static void resync_clock(void);

static const unsigned char sine14[14] = {
    128, 171, 206, 225, 225, 206, 171, 128, 85, 50, 31, 31, 50, 85
};

/* ------------------------------------------------------------------ */
/* Timer interrupt: the Wolf3D SDL_SSService() sequence, 700 Hz.       */
/* ------------------------------------------------------------------ */

static void interrupt isr8(void)
{
    unsigned n;
    unsigned prev;

    isr_count++;

    if (ss_on) {
        n = 0;
        while (n < MAX_BURST && (inportb(base_port + REG_STATUS) & STATUS_FULL) == 0) {
            outportb(base_port + REG_DATA, tone[tpos]);
            if (++tpos >= TONE_LEN) tpos = 0;
            outportb(base_port + REG_CONTROL, DSS_CTRL_STROBE);
            inportb(0x80);                       /* push/pop stand-in */
            outportb(base_port + REG_CONTROL, DSS_CTRL_ON);
            inportb(0x80);
            inportb(0x80);
            n++;
            strobes++;
        }
        if (n >= MAX_BURST) capped++;
    }

    /* Keep the DOS clock right: hand every 65536/PIT_DIV-th interrupt to
     * the BIOS handler (which sends its own EOI), EOI the rest here. */
    prev = acc;
    acc += PIT_DIV;
    if (acc < prev) {
        old8();
    } else {
        outportb(0x20, 0x20);
    }
}

static void install_isr(void)
{
    if (installed) return;
    old8 = getvect(8);
    disable();
    setvect(8, isr8);
    outportb(0x43, 0x36);
    outportb(0x40, (unsigned char)(PIT_DIV & 0xFFu));
    outportb(0x40, (unsigned char)(PIT_DIV >> 8));
    enable();
    installed = 1;
}

static void uninstall_isr(void)
{
    if (!installed) return;
    disable();
    outportb(0x43, 0x36);
    outportb(0x40, 0);
    outportb(0x40, 0);
    setvect(8, old8);
    enable();
    installed = 0;
    if (slip_secs) {
        /* Self-test: pretend the interrupts-off windows cost this many
         * seconds, then let resync_clock() put it right and report it. */
        biostime(1, biostime(0, 0L) - (long)slip_secs * 18L);
    }
    resync_clock();
}

/* ------------------------------------------------------------------ */

static void ctrl(unsigned char v)
{
    outportb(base_port + REG_CONTROL, v);
}

static void dss_power_up(void)
{
    ctrl(DSS_CTRL_ON);
    delay(5);
}

static void dss_finish(void)
{
    ctrl(DSS_CTRL_STROBE);
    delay(2);
    ctrl(TRINKET_CTRL_IDLE);
    delay(2);
}

static void cleanup(void)
{
    ss_on = 0;
    uninstall_isr();
    ctrl(TRINKET_CTRL_IDLE);
}

static int ctrlbrk_handler(void)
{
    g_abort = 1;
    return 1;
}

/* DOS critical error (Abort/Retry/Ignore): count it and fail the call
 * instead of prompting. */
static int crit_handler(int errval, int ax, int bp, int si)
{
    (void)errval; (void)ax; (void)bp; (void)si;
    crit_errors++;
    hardretn(-1);
    return 0;
}

static int stop_requested(void)
{
    if (g_abort) return 1;
    if (kbhit()) {
        getch();
        return 1;
    }
    return 0;
}

/* Seconds since midnight from the AT real-time clock, or -1 if there is
 * none (XT-class machines). Used only to count lost timer ticks. */
static long rtc_seconds(void)
{
    union REGS r;
    r.h.ah = 2;
    int86(0x1A, &r, &r);
    if (r.x.cflag) return -1L;
#define BCD(x) ((((x) >> 4) & 0x0F) * 10 + ((x) & 0x0F))
    return (long)BCD(r.h.ch) * 3600L + (long)BCD(r.h.cl) * 60L + (long)BCD(r.h.dh);
#undef BCD
}

/* The DOS clock is the BIOS tick count, and our timer handler only hands
 * the BIOS every 38th-or-so INTERRUPT IT SERVICED. Every interrupt lost to
 * an interrupts-off window is a tick the clock never sees, so after a run
 * the DOS time is behind by roughly the whole run. Put it right from the
 * AT real-time clock when there is one (and report how far it slipped). */
static void resync_clock(void)
{
    long secs = rtc_seconds();
    unsigned long want, have_secs;
    long bios_ticks;

    if (secs < 0L) {
        g_clock_state = 2;
        return;
    }
    /* BIOS ticks run at 18.2065 Hz = 182065/10000. Split the multiply so it
     * stays inside 32 bits (a full day is ~1.57 million ticks): a rounded
     * constant like 18207 is off by about a second by mid-afternoon. */
    want = ((unsigned long)secs / 10000UL) * 182065UL
         + (((unsigned long)secs % 10000UL) * 182065UL) / 10000UL;
    bios_ticks = biostime(0, 0L);
    have_secs = ((unsigned long)bios_ticks / 182065UL) * 10000UL
              + (((unsigned long)bios_ticks % 182065UL) * 10000UL) / 182065UL;
    g_clock_behind = secs - (long)have_secs;
    biostime(1, (long)want);
    g_clock_state = 1;
}

/* ------------------------------------------------------------------ */
/* CRC-32 (IEEE), table built at start-up                              */
/* ------------------------------------------------------------------ */

static unsigned long crc_table[256];

static void crc_init(void)
{
    unsigned i, k;
    unsigned long c;
    for (i = 0; i < 256; i++) {
        c = (unsigned long)i;
        for (k = 0; k < 8; k++) {
            c = (c & 1UL) ? (0xEDB88320UL ^ (c >> 1)) : (c >> 1);
        }
        crc_table[i] = c;
    }
}

static unsigned long crc_update(unsigned long crc, const unsigned char *p, unsigned n)
{
    while (n--) {
        crc = crc_table[(unsigned)((crc ^ *p++) & 0xFFu)] ^ (crc >> 8);
    }
    return crc;
}

/* ------------------------------------------------------------------ */
/* read mode                                                           */
/* ------------------------------------------------------------------ */

static unsigned char iobuf[16384];
static const unsigned block_sizes[4] = { 512, 2048, 8192, 16384 };

/* Returns 0 on success; -1 open failed, -2 read error / DOS error. */
static int crc_file(const char *path, unsigned bs, unsigned long *crc_out,
                    unsigned long *len_out, int inject)
{
    int fd;
    int n;
    unsigned long crc = 0xFFFFFFFFUL;
    unsigned long total = 0;
    int injected = 0;
    unsigned before = crit_errors;

    fd = open(path, O_RDONLY | O_BINARY);
    if (fd < 0) return -1;
    for (;;) {
        if (g_abort) break;
        n = read(fd, iobuf, bs);
        if (n < 0) { close(fd); return -2; }
        if (n == 0) break;
        if (inject && !injected && total > 0) {
            iobuf[0] ^= 0x5A;               /* deliberate corruption */
            injected = 1;
        }
        crc = crc_update(crc, iobuf, (unsigned)n);
        total += (unsigned long)n;
    }
    close(fd);
    if (crit_errors != before) return -2;
    *crc_out = ~crc;
    *len_out = total;
    return 0;
}

static void print_isr_summary(long t0, long t1, unsigned long isr0)
{
    unsigned long n = isr_count - isr0;
    printf("  feeder: %lu timer interrupts, %lu strobes", n, strobes);
    if (capped) printf(", %lu hit the %u-byte burst cap", capped, (unsigned)MAX_BURST);
    printf("\n");
    if (t0 >= 0 && t1 > t0 && (t1 - t0) >= 10L) {
        unsigned long expect = ((unsigned long)(t1 - t0) * PIT_HZ_X10) / 10UL;
        long lost = (long)expect - (long)n;
        printf("  timer interrupts serviced: %lu of ~%lu expected over %ld s (RTC) -> %ld lost (%ld%%)\n",
               n, expect, (long)(t1 - t0), lost,
               (expect > 0UL) ? (lost * 100L) / (long)expect : 0L);
    } else if (t0 < 0) {
        printf("  (no RTC on this machine, so lost timer ticks aren't measured)\n");
    } else {
        printf("  (run shorter than 10 s, so lost timer ticks aren't measured)\n");
    }
    if (g_clock_state == 1) {
        printf("  DOS clock had slipped %ld s behind during the run; resynced from the RTC.\n",
               g_clock_behind);
    } else if (g_clock_state == 2) {
        printf("  No RTC on this machine, so the DOS clock was NOT resynced and is now behind: reboot or set TIME.\n");
    }
}

static int cmd_read(const char *path, unsigned passes, int inject)
{
    unsigned long base_crc = 0, base_len = 0, crc, len;
    unsigned long isr0;
    unsigned pass, bs;
    unsigned bad = 0, errors = 0, ok = 0;
    int rc;
    long t0, t1;

    printf("DSS stress (read): %s, %u passes on port %03Xh (any key stops)\n",
           path, passes, base_port);

    rc = crc_file(path, 4096, &base_crc, &base_len, 0);
    if (rc != 0 || base_len == 0UL) {
        printf("FAIL: cannot read the baseline of '%s' (%s)\n", path,
               rc == -1 ? "open failed" : (rc == -2 ? "read/DOS error" : "empty file"));
        return 3;
    }
    printf("  baseline (no feeder): %lu bytes, CRC-32 %08lX\n", base_len, base_crc);
    if (base_len < 65536UL) {
        printf("  note: file is under 64 KB; a bigger one makes a much better test.\n");
    }

    dss_power_up();
    isr0 = isr_count;
    strobes = 0;
    capped = 0;
    install_isr();
    ss_on = 1;
    t0 = rtc_seconds();

    for (pass = 1; pass <= passes && !stop_requested(); pass++) {
        bs = block_sizes[pass & 3u];
        rc = crc_file(path, bs, &crc, &len, inject && pass == 2);
        if (g_abort) break;
        if (rc != 0) {
            errors++;
            printf("  pass %u (%u-byte reads): %s\n", pass, bs,
                   rc == -1 ? "open FAILED" : "read/DOS error");
        } else if (len != base_len || crc != base_crc) {
            bad++;
            printf("  pass %u (%u-byte reads): MISMATCH  length %lu CRC %08lX (want %lu / %08lX)\n",
                   pass, bs, len, crc, base_len, base_crc);
        } else {
            ok++;
        }
    }

    t1 = rtc_seconds();
    ss_on = 0;
    uninstall_isr();
    dss_finish();

    printf("  passes clean: %u   mismatches: %u   errors: %u   DOS critical errors: %u\n",
           ok, bad, errors, crit_errors);
    print_isr_summary(t0, t1, isr0);
    if (strobes == 0UL) {
        printf("  WARNING: the feeder never wrote a byte -- nAck stayed high or no DSS device answered.\n");
    }
    if (bad || errors || crit_errors) {
        printf("RESULT: FAIL\n");
        return 1;
    }
    printf("RESULT: PASS\n");
    return 0;
}

/* ------------------------------------------------------------------ */
/* write mode                                                          */
/* ------------------------------------------------------------------ */

static unsigned long lcg_state;

static void lcg_seed(void) { lcg_state = 0x1234ABCDUL; }

static void lcg_fill(unsigned char *p, unsigned n)
{
    while (n--) {
        lcg_state = lcg_state * 1103515245UL + 12345UL;
        *p++ = (unsigned char)((lcg_state >> 16) & 0xFFu);
    }
}

static int cmd_write(const char *path, unsigned kb, int inject)
{
    int fd;
    int n;
    unsigned long total, done, pos;
    unsigned long isr0;
    unsigned chunk;
    unsigned long mism = 0, first = 0xFFFFFFFFUL;
    unsigned write_errors = 0;
    unsigned char ref[4096];
    unsigned i;
    long t0, t1;
    int rc = 0;

    if (kb == 0) kb = 128;
    total = (unsigned long)kb * 1024UL;
    printf("DSS stress (write): %s, %u KB on port %03Xh (any key stops)\n", path, kb, base_port);
    printf("  this writes to the SD card and deletes the file afterwards.\n");

    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_BINARY, S_IREAD | S_IWRITE);
    if (fd < 0) {
        printf("FAIL: cannot create '%s'\n", path);
        return 3;
    }

    dss_power_up();
    isr0 = isr_count;
    strobes = 0;
    capped = 0;
    install_isr();
    ss_on = 1;
    t0 = rtc_seconds();

    lcg_seed();
    done = 0UL;
    while (done < total && !stop_requested()) {
        chunk = (total - done > 4096UL) ? 4096u : (unsigned)(total - done);
        lcg_fill(iobuf, chunk);
        n = write(fd, iobuf, chunk);
        if (n != (int)chunk) { write_errors++; break; }
        done += chunk;
    }
    close(fd);

    t1 = rtc_seconds();
    ss_on = 0;
    uninstall_isr();
    dss_finish();

    /* Read back with the feeder off and compare to the regenerated pattern. */
    fd = open(path, O_RDONLY | O_BINARY);
    if (fd < 0) {
        printf("FAIL: cannot reopen '%s' to verify\n", path);
        return 1;
    }
    lcg_seed();
    pos = 0UL;
    while (pos < done) {
        chunk = (done - pos > 4096UL) ? 4096u : (unsigned)(done - pos);
        n = read(fd, iobuf, chunk);
        if (n != (int)chunk) { printf("  short read at offset %lu\n", pos); mism++; break; }
        lcg_fill(ref, chunk);
        if (inject && pos == 0UL) iobuf[0] ^= 0x5A;
        for (i = 0; i < chunk; i++) {
            if (iobuf[i] != ref[i]) {
                if (first == 0xFFFFFFFFUL) first = pos + i;
                mism++;
            }
        }
        pos += chunk;
    }
    close(fd);
    unlink(path);

    printf("  wrote %lu bytes, verified %lu: %lu bad byte(s)", done, pos, mism);
    if (mism && first != 0xFFFFFFFFUL) printf(", first at offset %lu", first);
    printf("\n  write errors: %u   DOS critical errors: %u\n", write_errors, crit_errors);
    print_isr_summary(t0, t1, isr0);
    if (strobes == 0UL) {
        printf("  WARNING: the feeder never wrote a byte -- nAck stayed high or no DSS device answered.\n");
    }
    if (mism || write_errors || crit_errors || done != total) rc = 1;
    printf("%s\n", rc ? "RESULT: FAIL" : "RESULT: PASS");
    return rc;
}

/* ------------------------------------------------------------------ */

static unsigned long parse_hex_ul(const char *s)
{
    unsigned long v = 0;
    sscanf(s, "%lx", &v);
    return v;
}

int main(int argc, char **argv)
{
    const char *cmd = NULL;
    const char *args[3];
    int nargs = 0;
    int inject = 0;
    int i;
    int rc;

    for (i = 1; i < argc; i++) {
        if ((argv[i][0] == '/' || argv[i][0] == '-') &&
            (argv[i][1] == 'P' || argv[i][1] == 'p') && argv[i][2] == '=') {
            base_port = (unsigned)parse_hex_ul(argv[i] + 3);
        } else if ((argv[i][0] == '/' || argv[i][0] == '-') &&
                   (strcmp(argv[i] + 1, "INJECT") == 0 || strcmp(argv[i] + 1, "inject") == 0)) {
            inject = 1;
        } else if ((argv[i][0] == '/' || argv[i][0] == '-') &&
                   (argv[i][1] == 'S' || argv[i][1] == 's') &&
                   (argv[i][2] == 'L' || argv[i][2] == 'l') &&
                   (argv[i][3] == 'I' || argv[i][3] == 'i') &&
                   (argv[i][4] == 'P' || argv[i][4] == 'p') && argv[i][5] == '=') {
            slip_secs = (unsigned)atoi(argv[i] + 6);
        } else if (cmd == NULL) {
            cmd = argv[i];
        } else if (nargs < 3) {
            args[nargs++] = argv[i];
        }
    }

    if (cmd == NULL || nargs < 1) {
        fprintf(stderr,
            "Trinket DSS stress test: game timer interrupt vs. SD transactions\n"
            "Usage: DSSSTRES read  <file> [passes]   [/P=xxxx] [/INJECT]\n"
            "       DSSSTRES write <file> [kilobytes] [/P=xxxx] [/INJECT]\n\n"
            "  read   re-reads an existing file on the trinket drive (>= 64 KB) with a\n"
            "         700 Hz Sound Source feeder running and checks it against a baseline\n"
            "  write  creates, verifies and DELETES a scratch file on the SD card\n"
            "  /INJECT  corrupt one byte on purpose to prove failures are reported\n"
            "  /SLIP=s  knock s seconds off the DOS clock to test the clock resync\n\n"
            "Exit code 0 = pass, 1 = failure detected, 3 = could not run.\n");
        return 3;
    }

    for (i = 0; i < TONE_LEN; i++) tone[i] = sine14[i % 14];
    crc_init();
    ctrlbrk(ctrlbrk_handler);
    harderr(crit_handler);
    atexit(cleanup);
    ctrl(TRINKET_CTRL_IDLE);

    if (strcmp(cmd, "read") == 0) {
        rc = cmd_read(args[0], (nargs >= 2 && atoi(args[1]) > 0) ? (unsigned)atoi(args[1]) : 20u, inject);
    } else if (strcmp(cmd, "write") == 0) {
        rc = cmd_write(args[0], (nargs >= 2) ? (unsigned)atoi(args[1]) : 128u, inject);
    } else {
        fprintf(stderr, "dssstres: unknown command '%s'. Run without arguments for help.\n", cmd);
        return 3;
    }
    return rc;
}
