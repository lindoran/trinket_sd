/*
 * dsscfg.c -- show or change the trinket's Disney Sound Source FIFO depth
 *             at run time. DOS, Borland C++ 3.1, small model.
 *
 * The trinket plays a Disney Sound Source for programs that write the
 * parallel port directly (see DSS.md). By default it presents the real
 * unit's 16-byte FIFO. A deeper FIFO lets such a program's timer interrupt,
 * on the occasions it gets to run between disk transactions, write further
 * ahead; the trinket then keeps that audio playing while TRINKET.SYS has
 * the port, which is what bridges the short dropouts when a game loads
 * data from the SD card mid-sound.
 *
 *   DSSCFG                     show the current setting
 *   DSSCFG <depth> [warmup]    set it (depth 16..512, warm-up 0..2000 ms)
 *   DSSCFG default             back to 16 / 250 ms
 *
 *   /P=378   port base (hex), default 378
 *   /R       reset the trinket first (normally not needed)
 *
 *   depth    bytes of FIFO presented. 16 is the real unit and the default;
 *            it changes nothing. About 250 covers a 34 ms disk transaction
 *            (7000 samples/s), 512 (the maximum) about 73 ms.
 *   warm-up  Programs detect a Sound Source by writing more than 16 bytes
 *            and expecting the "full" flag. So a program that has just
 *            powered the unit up -- or has been away for a second or more --
 *            sees the real 16-byte FIFO for this many milliseconds before
 *            the deeper one takes over. 250 is plenty for the detection
 *            sequence Wolfenstein 3D uses; 0 turns the window off.
 *
 * The setting lives in the trinket's RAM. A power-cycle of the trinket
 * returns it to 16; TRINKET.SYS re-sends the value from its /f= and /w=
 * switches every time it initialises the card, so put your preferred depth
 * there (DEVICE=TRINKET.SYS ... /f=256) and use this tool to experiment.
 * A host reset does not clear it.
 *
 * This talks to the trinket directly, like DACTEST and DSSTEST do, with
 * interrupts held off for each short transaction. Run it from the DOS
 * prompt, not while a Sound Source program is running.
 *
 * Exit code: 0 = done, 1 = the trinket did not answer, 3 = bad usage.
 *
 * C89 only (declarations at the top of blocks), same constraints as
 * dactest.c -- see its header comment.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dos.h>

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

#define DISPATCH_CMD_DSS    0x05u
#define DSS_OP_SET          0x01u

#define DEPTH_MIN           16u
#define DEPTH_MAX           512u
#define WARMUP_MAX          2000u
#define WARMUP_DEFAULT      250u

static unsigned base_port = 0x378;
static unsigned char ctrl_shadow = CTRL_IDLE_DEFAULT;

static void ctrl_out(unsigned char value)
{
    ctrl_shadow = value;
    outportb(base_port + REG_CONTROL, value);
}

static void io_delay(void)
{
    int i;
    for (i = 0; i < IO_DELAY_ITERS; i++) {
        inportb(0x80);
    }
}

static void reset_hold_delay(void)
{
    int i;
    for (i = 0; i < 200; i++) {
        io_delay();
    }
}

static int wait_error_high(void)
{
    unsigned long i;
    for (i = 0; i < POLL_TIMEOUT_ITERS; i++) {
        if (inportb(base_port + REG_STATUS) & 0x08) return 0;
    }
    return 1;
}

static int wait_error_low(void)
{
    unsigned long i;
    for (i = 0; i < POLL_TIMEOUT_ITERS; i++) {
        if (!(inportb(base_port + REG_STATUS) & 0x08)) return 0;
    }
    return 1;
}

static void host_select_direction(int want_write)
{
    unsigned char v = (unsigned char)(ctrl_shadow & ~CTRL_SELECT_READ);
    v = (unsigned char)(v | (want_write ? CTRL_SELECT_WRITE : CTRL_SELECT_READ));
    ctrl_out(v);
    io_delay();
}

static void host_reset(void)
{
    ctrl_out((unsigned char)(CTRL_AUTOLF_ASSERT | CTRL_INIT_DROPPED | CTRL_SELECT_WRITE));
    reset_hold_delay();
    ctrl_out(CTRL_IDLE_DEFAULT);
    io_delay();
}

static int raw_send_byte(unsigned char byte)
{
    outportb(base_port + REG_DATA, byte);

    ctrl_out((unsigned char)(ctrl_shadow | CTRL_INIT_RAISED));
    if (wait_error_high()) return 1;

    ctrl_out((unsigned char)(ctrl_shadow & ~CTRL_INIT_RAISED));
    if (wait_error_low()) return 1;

    return 0;
}

/* The firmware pre-corrects the PC port's inversion of Busy (status bit 7),
 * so what is read here is the byte as sent. */
static int raw_recv_byte(unsigned char *out)
{
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

/* Interrupts off for the duration of one transaction, flags restored (not
 * blindly re-enabled) afterwards -- same reasoning as the driver's wire lock. */
static unsigned lock_irq(void)
{
    unsigned fl = _FLAGS;
    disable();
    return fl;
}

static void unlock_irq(unsigned fl)
{
    if (fl & 0x0200u) enable();
}

/* ------------------------------------------------------------------ */

/* [CMD_DSS][OP_SET][depth lo,hi][warmup lo,hi]  -- 0 on success */
static int dss_set(unsigned depth, unsigned warmup)
{
    unsigned fl;
    int rc;

    fl = lock_irq();
    host_select_direction(1);
    rc = raw_send_byte(DISPATCH_CMD_DSS);
    if (!rc) rc = raw_send_byte(DSS_OP_SET);
    if (!rc) rc = raw_send_byte((unsigned char)(depth & 0xFFu));
    if (!rc) rc = raw_send_byte((unsigned char)(depth >> 8));
    if (!rc) rc = raw_send_byte((unsigned char)(warmup & 0xFFu));
    if (!rc) rc = raw_send_byte((unsigned char)(warmup >> 8));
    ctrl_out(CTRL_IDLE_DEFAULT);
    unlock_irq(fl);
    return rc;
}

/* [CMD_DSS] then switch to read: status, depth, warm-up, max depth -- 0 on success */
static int dss_get(unsigned *depth, unsigned *warmup, unsigned *maxdepth, unsigned *status)
{
    unsigned char b[7];
    unsigned fl;
    int rc;
    int i;

    fl = lock_irq();
    host_select_direction(1);
    rc = raw_send_byte(DISPATCH_CMD_DSS);
    if (!rc) {
        host_select_direction(0);
        for (i = 0; i < 7 && !rc; i++) {
            rc = raw_recv_byte(&b[i]);
        }
    }
    host_select_direction(1);
    ctrl_out(CTRL_IDLE_DEFAULT);
    unlock_irq(fl);

    if (rc) return rc;
    *status   = b[0];
    *depth    = (unsigned)(b[1] | ((unsigned)b[2] << 8));
    *warmup   = (unsigned)(b[3] | ((unsigned)b[4] << 8));
    *maxdepth = (unsigned)(b[5] | ((unsigned)b[6] << 8));
    return 0;
}

static unsigned long parse_hex_ul(const char *s)
{
    unsigned long v = 0;
    sscanf(s, "%lx", &v);
    return v;
}

static void usage(void)
{
    fprintf(stderr,
        "Trinket Sound Source FIFO depth\n"
        "Usage: DSSCFG                    show the current setting\n"
        "       DSSCFG <depth> [warmup]   set it (depth 16-512, warm-up 0-2000 ms)\n"
        "       DSSCFG default            back to 16 / 250 ms\n"
        "       /P=xxxx  port base (hex)    /R  reset the trinket first\n\n"
        "16 is the real Sound Source's FIFO and changes nothing. A deeper FIFO lets a\n"
        "game's timer interrupt write further ahead, so the trinket can keep playing\n"
        "through a disk transaction (about 250 covers 34 ms; 512 is the maximum).\n"
        "Warm-up is how long a freshly powered-up program sees the real 16 bytes\n"
        "first so its presence test still passes. Put the value you settle on in\n"
        "CONFIG.SYS: DEVICE=TRINKET.SYS ... /f=<depth> /w=<warmup>.\n");
}

int main(int argc, char **argv)
{
    unsigned depth = 0, warmup = 0, maxdepth = 0, status = 0;
    unsigned want_depth = 0, want_warmup = WARMUP_DEFAULT;
    int have_set = 0, do_reset = 0;
    int nargs = 0;
    const char *args[3];
    int i;
    long v;

    for (i = 1; i < argc; i++) {
        if ((argv[i][0] == '/' || argv[i][0] == '-') &&
            (argv[i][1] == 'P' || argv[i][1] == 'p') && argv[i][2] == '=') {
            base_port = (unsigned)parse_hex_ul(argv[i] + 3);
        } else if ((argv[i][0] == '/' || argv[i][0] == '-') &&
                   (argv[i][1] == 'R' || argv[i][1] == 'r') && argv[i][2] == '\0') {
            do_reset = 1;
        } else if ((argv[i][0] == '/' || argv[i][0] == '-') &&
                   (argv[i][1] == '?' || argv[i][1] == 'h' || argv[i][1] == 'H')) {
            usage();
            return 3;
        } else if (nargs < 3) {
            args[nargs++] = argv[i];
        }
    }

    if (nargs >= 1) {
        if (strcmp(args[0], "default") == 0 || strcmp(args[0], "DEFAULT") == 0) {
            want_depth = DEPTH_MIN;
            want_warmup = WARMUP_DEFAULT;
            have_set = 1;
        } else {
            v = atol(args[0]);
            if (v < (long)DEPTH_MIN || v > (long)DEPTH_MAX) {
                fprintf(stderr, "dsscfg: depth must be %u-%u\n", DEPTH_MIN, DEPTH_MAX);
                return 3;
            }
            want_depth = (unsigned)v;
            if (nargs >= 2) {
                v = atol(args[1]);
                if (v < 0L || v > (long)WARMUP_MAX) {
                    fprintf(stderr, "dsscfg: warm-up must be 0-%u ms\n", WARMUP_MAX);
                    return 3;
                }
                want_warmup = (unsigned)v;
            }
            have_set = 1;
        }
    }

    ctrl_out(CTRL_IDLE_DEFAULT);
    if (do_reset) {
        host_reset();
    }

    if (have_set) {
        if (dss_set(want_depth, want_warmup) != 0) {
            fprintf(stderr, "dsscfg: the trinket did not answer on port %03Xh "
                            "(is a Sound Source program still holding the port?)\n", base_port);
            return 1;
        }
    }

    if (dss_get(&depth, &warmup, &maxdepth, &status) != 0) {
        fprintf(stderr, "dsscfg: the trinket did not answer on port %03Xh "
                        "(is a Sound Source program still holding the port?)\n", base_port);
        return 1;
    }

    printf("Trinket Sound Source FIFO: depth %u (range %u-%u), warm-up %u ms\n",
           depth, DEPTH_MIN, maxdepth, warmup);
    if (depth <= DEPTH_MIN) {
        printf("  16 = the real unit's FIFO (the default); nothing is buffered ahead.\n");
    } else {
        printf("  about %u ms of audio can be queued ahead of a disk transaction.\n",
               (unsigned)(((unsigned long)(depth - 1u) * 1000UL) / 7000UL));
    }
    if (have_set && (depth != want_depth || warmup != want_warmup)) {
        printf("  (clamped from %u / %u ms)\n", want_depth, want_warmup);
    }
    if (status != 0u) {
        printf("  warning: the trinket reported status %u\n", status);
    }
    return 0;
}
