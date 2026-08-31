/*
 * sdtest.c -- standalone SD-over-SPI-over-trinket diagnostic, built for
 * ia16-elf-gcc. Same relationship to the SPI/FAT16 work that
 * ifacetest.c has to the wire protocol: a plain, non-resident .EXE that
 * bypasses TRINKET.SYS entirely and talks straight to the parallel
 * port, so the edit-run loop while debugging SD command sequencing and
 * FAT16 parsing doesn't involve a driver, a TSR, or a reboot -- just
 * rebuild and rerun. This is deliberately step 1 of the block-device
 * work (see README.md's roadmap): prove SD bring-up + sector I/O +
 * FAT16 parsing here first, THEN move the proven logic into the
 * INT 2Fh redirector TSR. Nothing in this file is resident or hooks
 * any interrupt vector.
 *
 * Talks to trinket's DISPATCH_CMD_SPI subsystem (src/subsys_spi.h) the
 * same way ifacetest.c talks to DISPATCH_CMD_TEST_HARNESS: raw port
 * I/O, own dispatch-byte injection, no driver involved. If this file's
 * SPI plumbing (dispatch_send_spi_op / spi_get_result below) doesn't
 * work, check it against DISPATCH_CMD_TEST_HARNESS with ifacetest.c
 * first -- that isolates "wire/dispatch problem" from "SPI-subsystem-
 * specific problem", same diagnostic order this project has used at
 * every previous layer.
 *
 * SD-over-SPI command sequencing (sd_init, sd_cmd, block read/write)
 * follows the standard, widely-documented SD-in-SPI-mode bring-up
 * (CMD0 -> CMD8 -> ACMD41 loop -> optional CMD58/CMD16), the same
 * sequence used by e.g. ChaN's FatFs mmc_spi.c reference and countless
 * SPI-mode SD drivers -- nothing novel here, just this project's own
 * port-I/O idiom underneath it.
 *
 * KNOWN PERFORMANCE LIMITATION, worth flagging before this logic moves
 * into the TSR: every single SPI byte here costs two full top-level
 * dispatch cycles over the parallel link (one Write for SPI_OP_XFER,
 * one Read to fetch the result -- see spi_byte()/spi_xfer()). Busy-wait
 * polling loops (sd_write_block()'s program-busy wait especially, which
 * can legitimately take a card up to a couple hundred milliseconds)
 * inherit that overhead directly. Fine for a diagnostic run once in a
 * while; likely worth a batched "poll N bytes, tell me which one
 * stopped being 0xFF" SPI op if/when this needs to be fast inside the
 * TSR.
 *
 * UNTESTED / UNBUILT: written by inspection only, same status
 * everything in this project carries before its first real build (see
 * README.md, driver/NOTES.md). No ia16-elf-gcc in the sandbox this was
 * written in -- the port-I/O and dispatch layers were checked by direct
 * comparison against ifacetest.c/backend.c's already-established
 * shapes; the SD-command and FAT16 layers were checked by compiling as
 * ordinary portable C against a stubbed-out port-I/O layer on the host
 * (catches type/logic bugs, proves nothing about real SD-card timing or
 * DOS-side memory-model correctness). Build it, then run it against
 * ifacetest.c's already-hardware-confirmed transport before trusting
 * any of the SD-specific logic.
 *
 * fastinit/fastread/fastwrite/fastbench (Section 2b/5c) ARE built and
 * hardware-confirmed as of this note -- fastbench measured ~17ms/block
 * against bench's ~23ms/block on real hardware, see subsys_sd.h and
 * this project's own notes for that comparison and why the gap isn't
 * bigger. raw_recv_bulk() (Section 1, ENHANCEMENTS.md point 4) and the
 * Makefile's -O2 for this target are NOT yet re-tested against that
 * same fastbench number -- written the same "checked by inspection,
 * matches the pre-existing per-byte logic exactly" way the rest of
 * this paragraph describes, still needs a real rebuild + rerun to see
 * whether either actually moves it. Also not yet re-tested since the
 * g_tx_buf/g_cmd_buf stack-to-static conversion below (real fix for a
 * real stack-overflow-class bug hit on hardware, same pattern as
 * TRINKET.C's own first bug -- see this project's notes): that
 * conversion and this file's point-4 work don't touch the same
 * functions, but both need a fresh build+run before either is trusted
 * together with the other.
 *
 * Usage:
 *   SDTEST.EXE <command> [args] [/P=xxxx]
 *     /P=xxxx   LPT base I/O port, hex, no prefix (default 378),
 *               same convention as TRINKET.SYS/backend.c/ifacetest.c.
 *     -v        Verbose: trace the raw status/len_lo/len_hi bytes each
 *               spi_get_result() call reads back, before they're
 *               combined into the response length. Added to chase the
 *               open "~130 phantom byte reads" investigation in
 *               spi_get_result()'s own comment -- use this on the next
 *               real run and compare the printed len against the
 *               firmware's own "SPI: read result status=... len=..."
 *               serial line for the same transfer.
 *
 *   Commands:
 *     init                    bring up the card, print type/capacity,
 *                             find and parse the FAT16 volume, print
 *                             its BPB summary.
 *     dir                     init, then list the root directory.
 *     cat <8.3name>           init, then dump a root-dir file's
 *                             contents to stdout.
 *     readsector <lba>        init (card only, no FAT16 parse), read
 *                             one physical 512-byte sector, hex-dump
 *                             it. <lba> is in hex.
 *     writesector <lba> -y    init (card only), write a fixed test
 *                             pattern to one physical sector. Requires
 *                             the literal "-y" to do anything -- see
 *                             this command's own comment below for why.
 *     bench [count] [lba]     init, then replay <count> (default 57,
 *                             decimal -- matches dir_log.txt's DIR
 *                             trace) single-block reads at <lba>
 *                             (default 0, hex) back-to-back, timed via
 *                             the BIOS tick counter, data discarded.
 *                             Isolates the SD-command layer's wire
 *                             cost from DOS's own FAT-walk and the
 *                             resident driver's dispatch path -- see
 *                             this command's own comment below.
 *
 * Exit codes: 0 success, 1 SD/SPI-layer failure (card didn't respond,
 * bad token, timeout -- see stderr message for which), 2 FAT16-layer
 * failure (no FAT16 volume found, file not found), 3 command-line
 * usage error.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* ===================================================================
 * Section 1: raw port I/O + wire-level handshakes.
 *
 * Same idiom as client/ifacetest.c's equivalent section (ctrl bit
 * meanings, io_delay, host_reset, wait_error_high/low,
 * host_select_direction) -- see that file's header comment for the
 * doc-section references these implement. Duplicated rather than
 * shared because there is still no shared header between ia16-elf-gcc
 * translation units in this project (see dispatch.h's namespace note)
 * and this file has no dependency on ifacetest.c beyond "happens to
 * need the same primitives."
 *
 * outportb/inportb are the one deliberate exception, not line-for-line
 * with ifacetest.c: this file hit real `ia16-elf-gcc -O2` assembler
 * errors ("operand type mismatch for `in'/`out'") from ifacetest.c's
 * original "Nd" (immediate-or-DX) constraint form once outportb/
 * inportb actually got inlined at every call site -- confirmed via
 * disassembly that ifacetest.c's own "Nd" form only ever compiles
 * clean because it's built without -O2, so outportb/inportb stay real,
 * separate, un-inlined `call`s there; the constraint is never actually
 * resolved in an inlined context on this toolchain. This file forces
 * the "d" (DX-only) alternative instead, confirmed via disassembly to
 * still generate real outb/inb against the real port value, with
 * io_delay()'s four dummy reads and every polling loop intact -- see
 * this project's own notes for the disassembly that confirmed this.
 * ifacetest.c's "Nd" form is fine as-is only because it's never built
 * with -O2 or anything else that would inline it; if that ever
 * changes, it needs this same fix first.
 * =================================================================== */

static inline void outportb(unsigned port, unsigned char value) {
    __asm__ volatile ("outb %%al, %%dx" : : "a"(value), "d"(port));
}

static inline unsigned char inportb(unsigned port) {
    unsigned char ret;
    __asm__ volatile ("inb %%dx, %%al" : "=a"(ret) : "d"(port));
    return ret;
}

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

static unsigned base_port;
static unsigned char ctrl_shadow = CTRL_IDLE_DEFAULT;
static int g_verbose = 0;   /* set by -v; see spi_get_result()'s trace output */

static void ctrl_out(unsigned char value) {
    ctrl_shadow = value;
    outportb(base_port + REG_CONTROL, value);
}

/* Number of dummy port 0x80 reads per io_delay() call. 4 is this
 * project's long-standing default (see ifacetest.c/backend.c) and
 * hasn't been shown to be more than needed on any hardware this has
 * actually run on -- it stays the default here. ENHANCEMENTS.md point
 * 4 suggests this is worth trying lower (1-2) on faster machines,
 * since raw_recv_bulk()'s per-byte cost includes two of these calls;
 * that's a real, cheap experiment to run against actual hardware (try
 * IO_DELAY_ITERS 2 or 1, rerun fastbench, watch for actual handshake
 * failures, not just a faster number) -- not something to guess at
 * from here without a way to see whether a given machine's parallel
 * port and this firmware's own polling loop still settle correctly at
 * a shorter delay. */
#define IO_DELAY_ITERS  4

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

/* One Init/Error byte-handshake cycle, host->device, direction already
 * Write. Same as ifacetest.c's raw_send_byte(). */
static int raw_send_byte(unsigned char byte) {
    outportb(base_port + REG_DATA, byte);

    ctrl_out((unsigned char)(ctrl_shadow | CTRL_INIT_RAISED));
    if (wait_error_high()) return 1;

    ctrl_out((unsigned char)(ctrl_shadow & ~CTRL_INIT_RAISED));
    if (wait_error_low()) return 1;

    return 0;
}

/* One byte, two-cycle nibble handshake, device->host, direction already
 * Read. Same shape as ifacetest.c's host_read_byte(), minus that
 * function's dispatch-byte send (callers here do their own, since which
 * dispatch command precedes it varies -- see Section 2). */
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

/* Bulk nibble read -- reads `count` bytes via the exact same per-byte
 * protocol as raw_recv_byte() above (copied verbatim: same 2 nibbles,
 * same full Init/Error handshake each, same bit patterns), as ONE
 * loop instead of `count` separate function calls. This is
 * ENHANCEMENTS.md point 4's "unrolled bulk nibble stream", in plain C
 * rather than hand-written inline assembly -- deliberately: there is
 * still no path to real ia16-elf-gcc/DOS/RP2040 hardware in the
 * environment this was written in (see this file's UNTESTED header
 * note), and hand-rolled asm that's subtly wrong in some register-
 * clobbering way is both easier to get wrong and harder to catch by
 * inspection than equivalent C -- the g_tx_buf/g_cmd_buf stack-to-
 * static fix elsewhere in this file is exactly this project's own
 * evidence that this file's logic needs a real build (and real
 * hardware) to fully trust, and asm raises that bar further, not
 * lowers it. Try adding -O2 to the Makefile's CFLAGS before reaching
 * for asm at all: this file's outportb/inportb/io_delay/ctrl_out are
 * all declared `static inline`, which plain GCC only honors above
 * -O0 -- at an un-optimized build, EVERY one of those is a real
 * CALL/RET, not the register-resident couple-of-instructions
 * ENHANCEMENTS.md's point 4 is describing. -O2 may already close most
 * of the gap this function is chasing, with none of hand-written
 * asm's risk; if a real build shows it doesn't, hand-written asm is a
 * reasonable next experiment -- but run that against real hardware,
 * not from here.
 *
 * buf may be NULL (discard -- see cmd_fastbench()'s use via
 * sd2_get_result()'s buf==NULL/bufcap==0 path): every byte is still
 * read off the wire either way, since the protocol has to stay in
 * lockstep regardless of whether the caller wants the value. Returns
 * 0 on success, 1 on the first handshake failure -- some prefix of
 * buf[] (if not NULL) may already be filled in that case, same
 * partial-progress behavior calling raw_recv_byte() in a loop would
 * have. */
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
 * Section 2: DISPATCH_CMD_SPI sub-protocol.
 *
 * Mirrors src/subsys_spi.h's framing exactly -- see that file for the
 * authoritative description. SPI_OP_* / SPI_STATUS_* values below MUST
 * match subsys_spi.h by hand, same "no shared header across these
 * toolchains" caveat dispatch.h/backend.c/ifacetest.c already document
 * for DISPATCH_CMD_TEST_HARNESS.
 * =================================================================== */

#define DISPATCH_CMD_SPI  0x02u

#define SPI_OP_CONFIG   0x01u
#define SPI_OP_CS       0x02u
#define SPI_OP_XFER     0x03u

#define SPI_STATUS_OK     0x00u
#define SPI_STATUS_ERROR  0x01u

#define SPI_MAX_XFER  520u   /* must match subsys_spi.c's SPI_MAX_XFER */

/* Issues one Write-direction SPI op: dispatch byte, then the opcode,
 * then two optional payload spans sent back-to-back (a fixed-size
 * header first, then a variable-length body -- SPI_OP_XFER's 2-byte
 * length header + N TX bytes is why this takes two spans instead of
 * one; pass n2=0/payload2=NULL for ops that are just a header).
 *
 * The opcode is a REQUIRED, explicit argument on purpose -- this used
 * to be a function that only took "bytes to send after the dispatch
 * byte", trusting each caller to remember to put its own opcode as the
 * first element of that array. spi_op_config() and spi_op_cs() both
 * forgot to, which desynced every transfer after the first one they
 * sent (see docs/CODEGEN_NOTES.md and this file's own git history for
 * the full trace of what that looked like on the wire -- confirmed
 * against a real hardware log, not hypothetical). Making the opcode a
 * separate parameter here means a caller physically cannot omit it
 * again; don't reintroduce a "just pass an array, first byte is the
 * opcode" version of this function. */
static int dispatch_send_spi_op(unsigned char opcode,
                                 const unsigned char *payload1, unsigned n1,
                                 const unsigned char *payload2, unsigned n2) {
    unsigned i;

    host_select_direction(1);
    if (raw_send_byte(DISPATCH_CMD_SPI)) return 1;
    if (raw_send_byte(opcode)) return 1;
    for (i = 0; i < n1; i++) {
        if (raw_send_byte(payload1[i])) return 1;
    }
    for (i = 0; i < n2; i++) {
        if (raw_send_byte(payload2[i])) return 1;
    }
    return 0;
}

/* Fetches the result of the most recently completed SPI_OP_XFER (see
 * subsys_spi.h's Read-direction framing). *out_status is only valid if
 * this returns 0. Truncates into buf/bufcap the same way subsys_spi.c's
 * own oversize handling does -- bytes beyond bufcap are still read off
 * the wire (to stay in lockstep) but dropped, so bufcap should normally
 * just be sizeof(your buffer) and large enough for whatever request
 * produced this result.
 *
 * RESOLVED (see docs/CODEGEN_NOTES.md): a real-hardware run showed
 * every byte read back here consistently XORed with 0x88 (status 0x00
 * arrived as 0x88, a length of 6 arrived as 0x8E, etc.) -- traced to
 * subsys_spi.c's device-side sender never applying the `^0x88 // doc
 * sec 7` correction that subsys_testharness_run() already does for
 * every Read-direction byte. That correction turned out to be a wire-
 * protocol requirement for the nibble-read mechanism itself, not
 * something specific to the test harness's own data as this file
 * originally assumed. Fixed on the firmware side (subsys_spi.c's
 * send_next_byte()) -- nothing needs to change here, since this
 * function was always doing pure raw reconstruction with no
 * correction of its own, same as ifacetest.c/backend.c's read paths
 * already do; the device is now expected to hand back the real value
 * directly. Requires a firmware rebuild + reflash to take effect,
 * not just a client rebuild. The -v trace below stays -- it's what
 * caught this and is worth keeping for the next thing that looks like
 * it. */
static int spi_get_result(unsigned char *out_status, unsigned char *buf,
                           unsigned bufcap, unsigned *out_len) {
    unsigned char status, len_lo, len_hi, b;
    unsigned len, i;

    host_select_direction(1);
    if (raw_send_byte(DISPATCH_CMD_SPI)) return 1;
    host_select_direction(0);

    if (raw_recv_byte(&status)) return 1;
    if (raw_recv_byte(&len_lo)) return 1;
    if (raw_recv_byte(&len_hi)) return 1;
    len = (unsigned)len_lo | ((unsigned)len_hi << 8);
    if (g_verbose) {
        fprintf(stderr, "sdtest: [trace] get_result raw status=0x%02X len_lo=0x%02X len_hi=0x%02X -> len=%u\n",
                status, len_lo, len_hi, len);
    }

    for (i = 0; i < len; i++) {
        if (raw_recv_byte(&b)) return 1;
        if (buf != NULL && i < bufcap) {
            buf[i] = b;
        }
    }

    *out_status = status;
    if (out_len != NULL) {
        *out_len = len;
    }
    return 0;
}

/* SPI_OP_CONFIG: mode 0-3, khz little-endian. */
static int spi_op_config(unsigned char mode, unsigned khz) {
    unsigned char payload[3];
    payload[0] = mode;
    payload[1] = (unsigned char)(khz & 0xFFu);
    payload[2] = (unsigned char)((khz >> 8) & 0xFFu);
    return dispatch_send_spi_op(SPI_OP_CONFIG, payload, 3, NULL, 0);
}

/* SPI_OP_CS: 1 = assert (select), 0 = deassert (release). */
static int spi_op_cs(int assert_level) {
    unsigned char payload[1];
    payload[0] = (unsigned char)(assert_level ? 1 : 0);
    return dispatch_send_spi_op(SPI_OP_CS, payload, 1, NULL, 0);
}

/* SPI_OP_XFER + immediate result fetch: sends len TX bytes, blocks for
 * the real SPI transaction on the device side, then reads the RX bytes
 * back into rx (rx may be NULL to discard, e.g. for command frames
 * whose echo nobody cares about). Returns 0 on success with *rx_len
 * set; 1 on any handshake failure; -1 if the device reported
 * SPI_STATUS_ERROR (oversize request -- see subsys_spi.h). */
static int spi_xfer(const unsigned char *tx, unsigned len,
                     unsigned char *rx, unsigned rxcap, unsigned *rx_len) {
    unsigned char hdr[2];
    unsigned char status;
    unsigned got_len;

    if (len > SPI_MAX_XFER) {
        fprintf(stderr, "sdtest: spi_xfer len=%u exceeds SPI_MAX_XFER=%u\n", len, SPI_MAX_XFER);
        return 1;
    }

    hdr[0] = (unsigned char)(len & 0xFFu);
    hdr[1] = (unsigned char)((len >> 8) & 0xFFu);

    if (dispatch_send_spi_op(SPI_OP_XFER, hdr, 2, tx, len)) return 1;

    if (spi_get_result(&status, rx, rxcap, &got_len)) return 1;
    if (status != SPI_STATUS_OK) return -1;
    if (rx_len != NULL) {
        *rx_len = got_len;
    }
    return 0;
}

/* Convenience wrapper for the extremely common "clock one byte, get one
 * byte back" case (SD command polling, dummy clocks, ...). Returns the
 * received byte, or 0xFF on any failure -- 0xFF is also what a card
 * sends while it has nothing to say, so callers already have to treat
 * "no useful response yet" and "link failure" the same way during
 * polling loops; anything that needs to tell them apart should call
 * spi_xfer() directly instead. */
static unsigned char spi_byte(unsigned char tx_byte) {
    unsigned char tx[1], rx[1];
    unsigned rx_len;

    tx[0] = tx_byte;
    if (spi_xfer(tx, 1, rx, 1, &rx_len) != 0 || rx_len != 1) {
        return 0xFF;
    }
    return rx[0];
}

/* ===================================================================
 * Section 2b: DISPATCH_CMD_SD sub-protocol -- the new high-level SD
 * dispatch (ENHANCEMENTS.md point 1, src/subsys_sd.h). Mirrors that
 * file's framing exactly, same "no shared header across toolchains,
 * values must match by hand" caveat as Section 2 above.
 *
 * The `fast*` commands (Section 5c) use ONLY this section -- never
 * Section 2's spi_xfer()/dispatch_send_spi_op() or Section 3's
 * sd_cmd()/sd_init()/sd_read_block() -- so a side-by-side run of e.g.
 * `bench` (old path, Section 2+3) against `fastbench` (new path, this
 * section) is a clean A/B of the two over the exact same card and
 * wiring: same LBA, same count, same BIOS-tick timing, only the wire
 * protocol underneath differs.
 * =================================================================== */

#define DISPATCH_CMD_SD  0x03u

#define SD_OP_INIT           0x01u
#define SD_OP_READ_SECTORS   0x02u
#define SD_OP_WRITE_SECTORS  0x03u
#define SD_OP_CHECK_MEDIA    0x04u

#define SD_STATUS_OK     0x00u
#define SD_STATUS_ERROR  0x01u

#define SD_MAX_SECTORS  60u   /* 60 sectors * 512 = 30KB, fits within 16-bit GCC 32KB limit */

/* Fetches the result of the most recently completed SD_OP_* (see
 * subsys_sd.h's Read-direction framing: status, is_v2, block_addressed,
 * len_lo, len_hi, then len data bytes). Same truncate-past-bufcap /
 * drain-everything-off-the-wire behavior as spi_get_result() above. */
static int sd2_get_result(unsigned char *out_status, int *out_is_v2,
                           int *out_block_addressed, unsigned char *buf,
                           unsigned bufcap, unsigned *out_len) {
    unsigned char status, is_v2, block_addressed, len_lo, len_hi;
    unsigned char hdr[5];
    unsigned len, i;

    host_select_direction(1);
    if (raw_send_byte(DISPATCH_CMD_SD)) return 1;
    host_select_direction(0);

    for (i = 0; i < 5; i++)
        if (raw_recv_byte(hdr + i)) return 1;
    status = hdr[0]; is_v2 = hdr[1]; block_addressed = hdr[2];
    len_lo = hdr[3]; len_hi = hdr[4];
    len = (unsigned)len_lo | ((unsigned)len_hi << 8);

    /* Bulk-read the payload via raw_recv_bulk() (see that function's
     * own comment) instead of one raw_recv_byte() call per byte.
     * Three cases, so a bufcap smaller than len (cmd_fastread()'s
     * fixed 512-byte g_cmd_buf against a multi-sector len, in
     * particular) still only costs two bulk calls, not a fallback to
     * per-byte:
     *   - buf==NULL or bufcap==0: nothing to keep, one bulk call
     *     discards all `len` bytes (cmd_fastbench()'s case, and the
     *     one that actually matters for re-measuring against the
     *     existing bench/fastbench numbers).
     *   - bufcap >= len: one bulk call captures everything.
     *   - 0 < bufcap < len: one bulk call captures the first bufcap
     *     bytes, a second discards the rest -- still drains the full
     *     response so the wire protocol stays in lockstep. */
    if (buf == NULL || bufcap == 0) {
        if (raw_recv_bulk(NULL, len)) return 1;
    } else if (bufcap >= len) {
        if (raw_recv_bulk(buf, len)) return 1;
    } else {
        if (raw_recv_bulk(buf, bufcap)) return 1;
        if (raw_recv_bulk(NULL, len - bufcap)) return 1;
    }

    *out_status = status;
    if (out_is_v2 != NULL) *out_is_v2 = is_v2 ? 1 : 0;
    if (out_block_addressed != NULL) *out_block_addressed = block_addressed ? 1 : 0;
    if (out_len != NULL) *out_len = len;
    return 0;
}

/* SD_OP_INIT -- runs the whole CMD0..ACMD41/CMD58/CMD16 bring-up on
 * the RP2040 itself, no per-SD-command round trip over the parallel
 * link (contrast Section 3's sd_init() below, which is 1-2 dispatch
 * round trips per SPI byte). Returns 0 on success (*out_is_v2/
 * *out_block_addressed set), 1 on handshake failure, -1 if the card
 * itself failed to init (see the firmware's USB-CDC log for which
 * step). */
static int sd2_init(int *out_is_v2, int *out_block_addressed) {
    unsigned char status;

    host_select_direction(1);
    if (raw_send_byte(DISPATCH_CMD_SD)) return 1;
    if (raw_send_byte(SD_OP_INIT)) return 1;

    if (sd2_get_result(&status, out_is_v2, out_block_addressed, NULL, 0, NULL)) return 1;
    return (status == SD_STATUS_OK) ? 0 : -1;
}

/* SD_OP_READ_SECTORS -- reads `count` consecutive sectors starting at
 * `lba` in ONE dispatch round trip, into buf (>= count*512 bytes).
 * count must be <= SD_MAX_SECTORS. Same return convention as
 * sd2_init(). */
static int sd2_read_sectors(unsigned long lba, unsigned count,
                             unsigned char *buf, unsigned bufcap) {
    unsigned char status;

    if (count > SD_MAX_SECTORS) {
        fprintf(stderr, "sdtest: sd2_read_sectors count=%u exceeds SD_MAX_SECTORS=%u\n", count, SD_MAX_SECTORS);
        return 1;
    }

    host_select_direction(1);
    if (raw_send_byte(DISPATCH_CMD_SD)) return 1;
    if (raw_send_byte(SD_OP_READ_SECTORS)) return 1;
    if (raw_send_byte((unsigned char)(lba & 0xFFu))) return 1;
    if (raw_send_byte((unsigned char)((lba >> 8) & 0xFFu))) return 1;
    if (raw_send_byte((unsigned char)((lba >> 16) & 0xFFu))) return 1;
    if (raw_send_byte((unsigned char)((lba >> 24) & 0xFFu))) return 1;
    if (raw_send_byte((unsigned char)count)) return 1;

    if (sd2_get_result(&status, NULL, NULL, buf, bufcap, NULL)) return 1;
    return (status == SD_STATUS_OK) ? 0 : -1;
}

/* SD_OP_WRITE_SECTORS -- writes `count` consecutive sectors starting
 * at `lba` from buf (count*512 bytes) in ONE dispatch round trip
 * (payload still sent as individual write handshakes -- there's no
 * bulk byte-stream primitive at the wire level, same as everything
 * else in this project). Same return convention as sd2_init(). */
static int sd2_write_sectors(unsigned long lba, unsigned count,
                              const unsigned char *buf) {
    unsigned char status;
    unsigned long total = (unsigned long)count * 512ul;
    unsigned long i;

    if (count > SD_MAX_SECTORS) {
        fprintf(stderr, "sdtest: sd2_write_sectors count=%u exceeds SD_MAX_SECTORS=%u\n", count, SD_MAX_SECTORS);
        return 1;
    }

    host_select_direction(1);
    if (raw_send_byte(DISPATCH_CMD_SD)) return 1;
    if (raw_send_byte(SD_OP_WRITE_SECTORS)) return 1;
    if (raw_send_byte((unsigned char)(lba & 0xFFu))) return 1;
    if (raw_send_byte((unsigned char)((lba >> 8) & 0xFFu))) return 1;
    if (raw_send_byte((unsigned char)((lba >> 16) & 0xFFu))) return 1;
    if (raw_send_byte((unsigned char)((lba >> 24) & 0xFFu))) return 1;
    if (raw_send_byte((unsigned char)count)) return 1;
    for (i = 0; i < total; i++) {
        if (raw_send_byte(buf[i])) return 1;
    }

    if (sd2_get_result(&status, NULL, NULL, NULL, 0, NULL)) return 1;
    return (status == SD_STATUS_OK) ? 0 : -1;
}

/* SD_OP_CHECK_MEDIA -- cheap "is the card still responding" probe (one
 * CMD13 on the RP2040 side, no 512-byte transfer). Returns 0 if the
 * card answered, -1 if it didn't (or no sd2_init() has succeeded this
 * session), 1 on handshake failure. Does NOT tell you whether it's
 * still the *same* card -- see subsys_sd.h's header comment. */
static int sd2_check_media(void) {
    unsigned char status;

    host_select_direction(1);
    if (raw_send_byte(DISPATCH_CMD_SD)) return 1;
    if (raw_send_byte(SD_OP_CHECK_MEDIA)) return 1;

    if (sd2_get_result(&status, NULL, NULL, NULL, 0, NULL)) return 1;
    return (status == SD_STATUS_OK) ? 0 : -1;
}

/* ===================================================================
 * Section 3: SD-in-SPI-mode command layer.
 * =================================================================== */

static int g_block_addressed = 0;   /* 1 = SDHC/SDXC (CMD17/24 arg is a
                                      * block number); 0 = SDSC (arg is
                                      * a byte address, arg = lba*512) */

/* Sends a 6-byte SD command frame and polls up to 8 bytes of 0xFF for
 * the R1 response (MSB clear). CRC is only meaningful for CMD0/CMD8 in
 * SPI mode (CRC checking is otherwise normally disabled); 0x01 elsewhere
 * is the conventional "don't care but valid stop bit" filler, same as
 * ChaN's mmc_spi.c reference uses. */
static unsigned char sd_cmd(unsigned char cmd, unsigned long arg) {
    unsigned char frame[6];
    unsigned char r1;
    int i;

    frame[0] = (unsigned char)(0x40u | cmd);
    frame[1] = (unsigned char)(arg >> 24);
    frame[2] = (unsigned char)(arg >> 16);
    frame[3] = (unsigned char)(arg >> 8);
    frame[4] = (unsigned char)(arg);
    if (cmd == 0)      frame[5] = 0x95u;
    else if (cmd == 8) frame[5] = 0x87u;
    else                frame[5] = 0x01u;

    if (spi_xfer(frame, 6, NULL, 0, NULL) != 0) {
        return 0xFF;
    }

    r1 = 0xFF;
    for (i = 0; i < 8; i++) {
        r1 = spi_byte(0xFF);
        if (!(r1 & 0x80u)) break;
    }
    return r1;
}

/* CMD55 prefix + CMD41 -- the "app command" form ACMD41 requires. */
static unsigned char sd_acmd41(unsigned long arg) {
    unsigned char r1 = sd_cmd(55, 0);
    if (r1 & 0x80u) return r1;   /* transport failure */
    return sd_cmd(41, arg);
}

/* Standard SPI-mode bring-up: CMD0 -> CMD8 (SDv2 probe) -> ACMD41 loop
 * -> CMD58 (SDv2 only, for the CCS/block-addressing bit) or CMD16
 * (SDv1/MMC, force 512-byte blocks). Returns 0 on success, negative on
 * failure (see stderr for which step). Leaves the clock switched up to
 * a normal operating rate and CS deasserted on return either way. */
static int sd_init(void) {
    unsigned char r1;
    unsigned char r7[4];
    int i;
    int is_v2 = 0;

    /* >=74 dummy clocks with CS high, card power-up requirement. 10
     * bytes = 80 clocks, comfortably over the spec minimum. */
    spi_op_config(0, 400);   /* mode 0, 400kHz -- SD-safe init rate */
    spi_op_cs(0);
    {
        unsigned char dummy[10];
        memset(dummy, 0xFF, sizeof(dummy));
        (void)spi_xfer(dummy, sizeof(dummy), NULL, 0, NULL);
    }

    spi_op_cs(1);
    for (i = 0; i < 10; i++) {
        r1 = sd_cmd(0, 0);
        if (r1 == 0x01u) break;
    }
    if (r1 != 0x01u) {
        fprintf(stderr, "sdtest: CMD0 failed, r1=0x%02X (no card / not responding)\n", r1);
        spi_op_cs(0);
        return -1;
    }

    r1 = sd_cmd(8, 0x1AAu);
    if (r1 == 0x01u) {
        /* R7: 4 more bytes, echo of the check pattern in the low byte. */
        unsigned char rx[4];
        unsigned char tx[4];
        memset(tx, 0xFF, sizeof(tx));
        if (spi_xfer(tx, 4, rx, 4, NULL) != 0) {
            fprintf(stderr, "sdtest: CMD8 R7 read failed\n");
            spi_op_cs(0);
            return -1;
        }
        memcpy(r7, rx, 4);
        if (r7[2] != 0x01u || r7[3] != 0xAAu) {
            fprintf(stderr, "sdtest: CMD8 pattern mismatch (got %02X%02X) -- unsupported card\n",
                    r7[2], r7[3]);
            spi_op_cs(0);
            return -1;
        }
        is_v2 = 1;
    } else if (r1 & 0x04u) {
        /* Illegal command -- SDv1 or MMC, not SDv2. Not an error. */
        is_v2 = 0;
    } else {
        fprintf(stderr, "sdtest: CMD8 unexpected r1=0x%02X\n", r1);
        spi_op_cs(0);
        return -1;
    }

    for (i = 0; i < 1000; i++) {
        r1 = sd_acmd41(is_v2 ? 0x40000000ul : 0);
        if (r1 == 0x00u) break;
        if (r1 != 0x01u) {
            fprintf(stderr, "sdtest: ACMD41 unexpected r1=0x%02X\n", r1);
            spi_op_cs(0);
            return -1;
        }
    }
    if (r1 != 0x00u) {
        fprintf(stderr, "sdtest: ACMD41 never left idle -- timeout\n");
        spi_op_cs(0);
        return -1;
    }

    if (is_v2) {
        unsigned char rx[4];
        unsigned char tx[4];
        r1 = sd_cmd(58, 0);
        if (r1 != 0x00u) {
            fprintf(stderr, "sdtest: CMD58 unexpected r1=0x%02X\n", r1);
            spi_op_cs(0);
            return -1;
        }
        memset(tx, 0xFF, sizeof(tx));
        if (spi_xfer(tx, 4, rx, 4, NULL) != 0) {
            fprintf(stderr, "sdtest: CMD58 OCR read failed\n");
            spi_op_cs(0);
            return -1;
        }
        g_block_addressed = (rx[0] & 0x40u) ? 1 : 0;   /* CCS bit */
    } else {
        r1 = sd_cmd(16, 512);
        if (r1 != 0x00u) {
            fprintf(stderr, "sdtest: CMD16 (set blocklen 512) failed, r1=0x%02X\n", r1);
            spi_op_cs(0);
            return -1;
        }
        g_block_addressed = 0;
    }

    spi_op_cs(0);
    spi_op_config(0, 4000);   /* mode 0, up to 4MHz for normal operation */

    printf("sdtest: card ready (%s, %s-addressed)\n",
           is_v2 ? "SDv2" : "SDv1/MMC",
           g_block_addressed ? "block" : "byte");
    return 0;
}

static unsigned long sd_arg_for_lba(unsigned long lba) {
    return g_block_addressed ? lba : (lba * 512ul);
}

static unsigned char g_tx_buf[512];
static unsigned char g_cmd_buf[512];

/* CMD17 READ_SINGLE_BLOCK. buf must be >= 512 bytes. */
static int sd_read_block(unsigned long lba, unsigned char *buf) {
    unsigned char r1, token;
    int i;
    unsigned char discard[2];

    spi_op_cs(1);
    r1 = sd_cmd(17, sd_arg_for_lba(lba));
    if (r1 != 0x00u) {
        fprintf(stderr, "sdtest: CMD17 lba=%lu r1=0x%02X\n", lba, r1);
        spi_op_cs(0);
        return -1;
    }

    token = 0xFF;
    for (i = 0; i < 8000; i++) {
        token = spi_byte(0xFF);
        if (token != 0xFF) break;
    }
    if (token != 0xFEu) {
        fprintf(stderr, "sdtest: CMD17 lba=%lu bad data token 0x%02X\n", lba, token);
        spi_op_cs(0);
        return -1;
    }

    memset(g_tx_buf, 0xFF, sizeof(g_tx_buf));
    if (spi_xfer(g_tx_buf, 512, buf, 512, NULL) != 0) {
        fprintf(stderr, "sdtest: CMD17 lba=%lu data read failed\n", lba);
        spi_op_cs(0);
        return -1;
    }
    (void)spi_xfer(g_tx_buf, 2, discard, 2, NULL);   /* trailing CRC, discarded */

    spi_op_cs(0);
    return 0;
}

/* CMD24 WRITE_BLOCK. buf must be >= 512 bytes. */
static int sd_write_block(unsigned long lba, const unsigned char *buf) {
    unsigned char r1, resp;
    unsigned char token[1];
    unsigned char crc[2];
    unsigned long i;

    spi_op_cs(1);
    r1 = sd_cmd(24, sd_arg_for_lba(lba));
    if (r1 != 0x00u) {
        fprintf(stderr, "sdtest: CMD24 lba=%lu r1=0x%02X\n", lba, r1);
        spi_op_cs(0);
        return -1;
    }

    token[0] = 0xFEu;
    if (spi_xfer(token, 1, NULL, 0, NULL) != 0) {
        spi_op_cs(0);
        return -1;
    }

    memcpy(g_tx_buf, buf, 512);
    if (spi_xfer(g_tx_buf, 512, NULL, 0, NULL) != 0) {
        fprintf(stderr, "sdtest: CMD24 lba=%lu data write failed\n", lba);
        spi_op_cs(0);
        return -1;
    }
    crc[0] = 0xFFu; crc[1] = 0xFFu;   /* dummy CRC, checking disabled */
    (void)spi_xfer(crc, 2, NULL, 0, NULL);

    resp = spi_byte(0xFF);
    if ((resp & 0x1Fu) != 0x05u) {
        fprintf(stderr, "sdtest: CMD24 lba=%lu data rejected, resp=0x%02X\n", lba, resp);
        spi_op_cs(0);
        return -1;
    }

    /* Busy-wait: card holds MISO low while programming. */
    for (i = 0; i < 60000ul; i++) {
        if (spi_byte(0xFF) != 0x00u) break;
    }

    spi_op_cs(0);
    return 0;
}

/* ===================================================================
 * Section 4: FAT16 layer -- boot sector / root directory / cluster
 * chains. Offsets are the standard FAT16 BPB/dirent layout; see e.g.
 * Microsoft's own published FAT spec for the field-by-field reference
 * -- not reproduced here beyond the offsets needed to read them.
 * =================================================================== */

static unsigned long g_vol_base_lba;      /* partition start, 0 if unpartitioned */
static unsigned g_bytes_per_sector;
static unsigned g_sectors_per_cluster;
static unsigned g_reserved_sectors;
static unsigned g_num_fats;
static unsigned g_root_entries;
static unsigned long g_fat_size_sectors;
static unsigned long g_root_dir_lba;      /* relative to g_vol_base_lba */
static unsigned long g_root_dir_sectors;
static unsigned long g_data_start_lba;    /* relative to g_vol_base_lba */

static unsigned char g_sector[512];

static int phys_read(unsigned long rel_lba, unsigned char *buf) {
    return sd_read_block(g_vol_base_lba + rel_lba, buf);
}

static unsigned rd_u16(const unsigned char *p) {
    return (unsigned)p[0] | ((unsigned)p[1] << 8);
}

static unsigned long rd_u32(const unsigned char *p) {
    return (unsigned long)p[0] | ((unsigned long)p[1] << 8)
         | ((unsigned long)p[2] << 16) | ((unsigned long)p[3] << 24);
}

/* Reads sector 0; if it's a FAT16 boot sector directly, volume starts
 * at LBA 0. Otherwise treats it as an MBR and takes partition entry 1's
 * start LBA, then re-reads and checks THAT sector. Returns 0 and fills
 * in the g_* globals on success. */
static int fat16_mount(void) {
    if (sd_read_block(0, g_cmd_buf) != 0) {
        fprintf(stderr, "sdtest: couldn't read sector 0\n");
        return -1;
    }

    if (g_cmd_buf[510] != 0x55u || g_cmd_buf[511] != 0xAAu) {
        fprintf(stderr, "sdtest: sector 0 has no 0x55AA boot signature\n");
        return -1;
    }

    if (memcmp(g_cmd_buf + 0x36, "FAT16", 5) == 0) {
        g_vol_base_lba = 0;
    } else {
        /* Treat as MBR, partition entry 1 at offset 0x1BE. */
        unsigned long part_lba = rd_u32(g_cmd_buf + 0x1BE + 8);
        if (part_lba == 0) {
            fprintf(stderr, "sdtest: sector 0 is neither a FAT16 boot sector nor a usable MBR\n");
            return -1;
        }
        if (sd_read_block(part_lba, g_cmd_buf) != 0) {
            fprintf(stderr, "sdtest: couldn't read partition start sector (lba=%lu)\n", part_lba);
            return -1;
        }
        if (g_cmd_buf[510] != 0x55u || g_cmd_buf[511] != 0xAAu || memcmp(g_cmd_buf + 0x36, "FAT16", 5) != 0) {
            fprintf(stderr, "sdtest: partition at lba=%lu is not FAT16\n", part_lba);
            return -1;
        }
        g_vol_base_lba = part_lba;
    }

    g_bytes_per_sector    = rd_u16(g_cmd_buf + 0x0B);
    g_sectors_per_cluster = g_cmd_buf[0x0D];
    g_reserved_sectors    = rd_u16(g_cmd_buf + 0x0E);
    g_num_fats            = g_cmd_buf[0x10];
    g_root_entries        = rd_u16(g_cmd_buf + 0x11);
    g_fat_size_sectors    = rd_u16(g_cmd_buf + 0x16);

    if (g_bytes_per_sector != 512) {
        fprintf(stderr, "sdtest: bytes/sector=%u not supported (only 512)\n", g_bytes_per_sector);
        return -1;
    }

    g_root_dir_lba     = g_reserved_sectors + (unsigned long)g_num_fats * g_fat_size_sectors;
    g_root_dir_sectors = ((unsigned long)g_root_entries * 32 + 511) / 512;
    g_data_start_lba   = g_root_dir_lba + g_root_dir_sectors;

    printf("sdtest: FAT16 volume at lba=%lu, %u sec/clus, %u FAT(s) x %lu sectors, "
           "%u root entries, data starts at lba+%lu\n",
           g_vol_base_lba, g_sectors_per_cluster, g_num_fats, g_fat_size_sectors,
           g_root_entries, g_data_start_lba);
    return 0;
}

/* Cluster N's first sector, relative to g_vol_base_lba. */
static unsigned long fat16_cluster_lba(unsigned cluster) {
    return g_data_start_lba + (unsigned long)(cluster - 2) * g_sectors_per_cluster;
}

/* Next cluster in the chain, or 0xFFFF+ (>=0xFFF8) at end-of-chain. */
static unsigned fat16_next_cluster(unsigned cluster) {
    unsigned long fat_byte_off = (unsigned long)cluster * 2;
    unsigned long fat_sector = g_reserved_sectors + fat_byte_off / 512;
    unsigned in_sector_off = (unsigned)(fat_byte_off % 512);

    if (phys_read(fat_sector, g_sector) != 0) {
        return 0xFFFFu;   /* treat a read failure as end-of-chain */
    }
    return rd_u16(g_sector + in_sector_off);
}

/* One root-directory entry, decoded from the raw 32 bytes. */
struct fat16_dirent {
    char name[13];   /* "NAME.EXT\0" form, trailing spaces trimmed */
    unsigned char attr;
    unsigned cluster;
    unsigned long size;
};

static void decode_dirent(const unsigned char *raw, struct fat16_dirent *out) {
    int i, n;

    n = 0;
    for (i = 0; i < 8 && raw[i] != ' '; i++) out->name[n++] = raw[i];
    if (raw[8] != ' ') {
        out->name[n++] = '.';
        for (i = 8; i < 11 && raw[i] != ' '; i++) out->name[n++] = raw[i];
    }
    out->name[n] = '\0';

    out->attr    = raw[0x0B];
    out->cluster = rd_u16(raw + 0x1A);
    out->size    = rd_u32(raw + 0x1C);
}

/* Calls cb(&dirent) for every live (non-deleted, non-LFN, non-volume-
 * label) root directory entry. Stops early if cb returns nonzero. */
static void fat16_list_root(int (*cb)(const struct fat16_dirent *)) {
    unsigned long sec;

    for (sec = 0; sec < g_root_dir_sectors; sec++) {
        unsigned i;

        if (phys_read(g_root_dir_lba + sec, g_sector) != 0) {
            fprintf(stderr, "sdtest: root dir sector %lu read failed\n", sec);
            return;
        }

        for (i = 0; i < 512 / 32; i++) {
            const unsigned char *raw = g_sector + i * 32;
            struct fat16_dirent de;

            if (raw[0] == 0x00u) return;         /* end of directory */
            if (raw[0] == 0xE5u) continue;        /* deleted */
            if (raw[0x0B] == 0x0Fu) continue;     /* LFN fragment */
            if (raw[0x0B] & 0x08u) continue;      /* volume label */

            decode_dirent(raw, &de);
            if (cb(&de)) return;
        }
    }
}

/* ===================================================================
 * Section 5: command handlers.
 * =================================================================== */

static int print_dirent(const struct fat16_dirent *de) {
    printf("%-13s %6lu  %s cluster=%u\n",
           de->name, de->size,
           (de->attr & 0x10u) ? "<DIR>" : "     ",
           de->cluster);
    return 0;
}

static struct fat16_dirent *g_find_target_result;
static const char *g_find_target_name;

static int find_target(const struct fat16_dirent *de) {
    if (strcmp(de->name, g_find_target_name) == 0) {
        *g_find_target_result = *de;
        return 1;
    }
    return 0;
}

static int cmd_init(void) {
    if (sd_init() != 0) return 1;
    if (fat16_mount() != 0) return 2;
    return 0;
}

static int cmd_dir(void) {
    if (sd_init() != 0) return 1;
    if (fat16_mount() != 0) return 2;
    fat16_list_root(print_dirent);
    return 0;
}

static int cmd_cat(const char *name) {
    struct fat16_dirent target;
    unsigned cluster;
    unsigned long remaining;
    char upname[13];
    int i;

    if (sd_init() != 0) return 1;
    if (fat16_mount() != 0) return 2;

    for (i = 0; name[i] != '\0' && i < 12; i++) {
        upname[i] = (char)toupper((unsigned char)name[i]);
    }
    upname[i] = '\0';

    g_find_target_result = &target;
    g_find_target_name = upname;
    target.name[0] = '\0';
    fat16_list_root(find_target);

    if (target.name[0] == '\0') {
        fprintf(stderr, "sdtest: %s not found\n", upname);
        return 2;
    }
    if (target.attr & 0x10u) {
        fprintf(stderr, "sdtest: %s is a directory\n", upname);
        return 2;
    }

    cluster = target.cluster;
    remaining = target.size;
    while (cluster >= 2 && cluster < 0xFFF8u && remaining > 0) {
        unsigned c;
        for (c = 0; c < g_sectors_per_cluster && remaining > 0; c++) {
            unsigned long chunk = (remaining < 512) ? remaining : 512;
            if (phys_read(fat16_cluster_lba(cluster) + c, g_sector) != 0) {
                fprintf(stderr, "sdtest: read failed mid-file\n");
                return 1;
            }
            fwrite(g_sector, 1, (size_t)chunk, stdout);
            remaining -= chunk;
        }
        cluster = fat16_next_cluster(cluster);
    }
    return 0;
}

static int cmd_readsector(unsigned long lba) {
    unsigned i;

    if (sd_init() != 0) return 1;
    if (sd_read_block(lba, g_cmd_buf) != 0) return 1;

    for (i = 0; i < 512; i += 16) {
        unsigned j;
        printf("%04X: ", i);
        for (j = 0; j < 16; j++) printf("%02X ", g_cmd_buf[i + j]);
        printf(" ");
        for (j = 0; j < 16; j++) {
            unsigned char c = g_cmd_buf[i + j];
            putchar((c >= 32 && c < 127) ? (char)c : '.');
        }
        putchar('\n');
    }
    return 0;
}

static int cmd_writesector(unsigned long lba, int confirmed) {
    unsigned i;

    if (!confirmed) {
        /* Writing a raw sector is destructive and this is a diagnostic
         * tool someone will be running a lot while iterating -- require
         * an explicit "-y" rather than let a typo'd lba silently
         * overwrite something on the card. */
        fprintf(stderr, "sdtest: writesector requires -y to confirm (this overwrites lba=%lu)\n", lba);
        return 3;
    }

    for (i = 0; i < 512; i++) {
        g_cmd_buf[i] = (unsigned char)(i & 0xFFu);
    }

    if (sd_init() != 0) return 1;
    if (sd_write_block(lba, g_cmd_buf) != 0) return 1;

    printf("sdtest: wrote test pattern to lba=%lu\n", lba);
    return 0;
}

/* ===================================================================
 * Section 5b: benchmark -- replay N single-block reads, time only.
 *
 * Purpose: isolate the wire-protocol round-trip cost dir_log.txt's
 * analysis pointed at, from everything a real DOS DIR also drags in
 * (DOS kernel FAT-walk decisions, TRINKET.SYS's own dispatch path,
 * any console I/O). This calls the exact same sd_read_block() as
 * `readsector` above and everything else in this file -- same
 * CS/CMD17/token-poll/512-byte-xfer/CRC-discard sequence a DIR's FAT
 * scan drives, just replayed count times back-to-back against a
 * single fixed LBA (address doesn't matter -- CMD17's handshake cost
 * is identical regardless of which sector it names) with the 512
 * bytes each read returns thrown away immediately rather than printed
 * or parsed. Not a substitute for timing a real DIR end-to-end: this
 * measures the SD-command layer's wire cost alone, not the resident
 * driver's own dispatch path or DOS's FAT-walk logic on top of it --
 * useful for isolating whether a firmware-side change (the ESC debug
 * toggle, a future CRC-skip, etc.) moved that specific number, not
 * for reproducing DOS's exact reported DIR time.
 *
 * Timing: BIOS INT 1Ah AH=00h (read system clock counter), which
 * ticks at ~18.2065 Hz (~54.9ms/tick) -- coarse next to a single
 * block's cost, fine at N=57+ blocks where the total run is several
 * seconds. AL on return is DOS's midnight-rollover flag; a nonzero
 * value from the closing read means midnight passed mid-run and the
 * elapsed figure below is wrong -- flagged rather than silently
 * trusted, since this runs standalone with no reason to assume a
 * time-of-day scheduler around it.
 * =================================================================== */

static unsigned long bios_ticks(unsigned char *midnight_flag) {
    unsigned short ax = 0x0000u;   /* AH=0x00 (read clock) in, AL=midnight flag out */
    unsigned short cx, dx;
    __asm__ volatile (
        "int $0x1a"
        : "+a"(ax), "=c"(cx), "=d"(dx)
        :
        : "cc"
    );
    if (midnight_flag) *midnight_flag = (unsigned char)(ax & 0xFFu);
    return ((unsigned long)cx << 16) | dx;
}

static int cmd_bench(unsigned long lba, unsigned count) {
    unsigned long t0, t1, elapsed_ticks, elapsed_ms;
    unsigned char midnight;
    unsigned i;

    if (sd_init() != 0) return 1;

    printf("sdtest: bench: %u single-block reads at lba=%lu (data discarded)\n", count, lba);

    t0 = bios_ticks(NULL);
    for (i = 0; i < count; i++) {
        if (sd_read_block(lba, g_cmd_buf) != 0) {
            fprintf(stderr, "sdtest: bench: read #%u failed, aborting\n", i);
            return 1;
        }
        /* g_cmd_buf intentionally untouched beyond this -- next iteration
         * overwrites it, nothing here reads or prints it. */
    }
    t1 = bios_ticks(&midnight);

    elapsed_ticks = t1 - t0;   /* wraps naturally if t1 < t0 within a day */
    elapsed_ms = (elapsed_ticks * 54925ul) / 1000ul;

    printf("sdtest: bench: %lu ticks (~%lu ms) for %u blocks, ~%lu ms/block\n",
           elapsed_ticks, elapsed_ms, count,
           count ? (elapsed_ms / count) : 0ul);
    if (midnight) {
        fprintf(stderr, "sdtest: bench: WARNING -- midnight rollover during run, timing above is unreliable\n");
    }

    return 0;
}

/* ===================================================================
 * Section 5c: `fast*` commands -- exercise DISPATCH_CMD_SD (Section
 * 2b) instead of the old DISPATCH_CMD_SPI + host-side SD layer
 * (Section 2/3). Deliberately parallel to init/readsector/writesector/
 * bench above (same argument shapes, same hex-dump style, same
 * writesector confirmation guard) so the two paths are easy to compare
 * command-for-command. No FAT16 layer here yet -- these are card-only,
 * matching readsector/bench's own scope, not `init`/`dir`/`cat`'s.
 * =================================================================== */

static int cmd_fastinit(void) {
    int is_v2 = 0, block_addressed = 0;
    int rc = sd2_init(&is_v2, &block_addressed);
    if (rc != 0) {
        fprintf(stderr, "sdtest: fastinit: SD_OP_INIT failed (rc=%d)\n", rc);
        return 1;
    }
    printf("sdtest: fastinit: card ready (%s, %s-addressed)\n",
           is_v2 ? "SDv2" : "SDv1/MMC", block_addressed ? "block" : "byte");
    return 0;
}

/* fastcheck -- exercises SD_OP_CHECK_MEDIA (ENHANCEMENTS.md point 3's
 * firmware-side half) on its own, without an init first, so it's also
 * a test of the "not initialized yet" path: run this as the very
 * first command after a fresh flash/reset and it should report
 * not-responding, not crash or hang. Run `fastinit` first to see it
 * flip to responding. */
static int cmd_fastcheck(void) {
    int rc = sd2_check_media();
    if (rc == 0) {
        printf("sdtest: fastcheck: card responding\n");
        return 0;
    }
    if (rc == -1) {
        fprintf(stderr, "sdtest: fastcheck: card NOT responding (or no successful fastinit/fastread/fastwrite/fastbench yet this session)\n");
        return 1;
    }
    fprintf(stderr, "sdtest: fastcheck: handshake failure\n");
    return 1;
}

static int cmd_fastread(unsigned long lba, unsigned count) {
    int is_v2 = 0, block_addressed = 0;
    unsigned i;

    if (count == 0) count = 1;
    if (sd2_init(&is_v2, &block_addressed) != 0) return 1;
    if (sd2_read_sectors(lba, count, g_cmd_buf, sizeof(g_cmd_buf)) != 0) {
        fprintf(stderr, "sdtest: fastread: SD_OP_READ_SECTORS failed\n");
        return 1;
    }

    printf("sdtest: fastread: %u sector(s) from lba=%lu ok, first sector:\n", count, lba);
    for (i = 0; i < 512; i += 16) {
        unsigned j;
        printf("%04X: ", i);
        for (j = 0; j < 16; j++) printf("%02X ", g_cmd_buf[i + j]);
        printf(" ");
        for (j = 0; j < 16; j++) {
            unsigned char c = g_cmd_buf[i + j];
            putchar((c >= 32 && c < 127) ? (char)c : '.');
        }
        putchar('\n');
    }
    return 0;
}

static int cmd_fastwrite(unsigned long lba, int confirmed) {
    unsigned i;
    int is_v2 = 0, block_addressed = 0;

    if (!confirmed) {
        /* Same guard as cmd_writesector() -- see that function's own
         * comment for why. */
        fprintf(stderr, "sdtest: fastwrite requires -y to confirm (this overwrites lba=%lu)\n", lba);
        return 3;
    }

    for (i = 0; i < 512; i++) g_cmd_buf[i] = (unsigned char)(i & 0xFFu);

    if (sd2_init(&is_v2, &block_addressed) != 0) return 1;
    if (sd2_write_sectors(lba, 1, g_cmd_buf) != 0) {
        fprintf(stderr, "sdtest: fastwrite: SD_OP_WRITE_SECTORS failed\n");
        return 1;
    }
    printf("sdtest: fastwrite: wrote test pattern to lba=%lu\n", lba);
    return 0;
}

/* fastbench -- direct counterpart to cmd_bench() (Section 5b), same
 * default count/lba, same BIOS-tick timing, so the two numbers are
 * directly comparable. The whole point of this one: cmd_bench() pays
 * a full DISPATCH_CMD_SPI round trip's worth of framing per SPI byte
 * inside each of its `count` separate sd_read_block() calls; this
 * issues ONE SD_OP_READ_SECTORS request for all `count` sectors and
 * gets ONE response back, with all `count` CMD17s happening on the
 * RP2040 side with no parallel-link traffic in between. If
 * ENHANCEMENTS.md point 1's "eliminates ~90% of parallel-port
 * handshake traffic" estimate holds, this should land well under
 * cmd_bench()'s ~22ms/block. */
static int cmd_fastbench(unsigned long lba, unsigned count) {
    unsigned long t0, t1, elapsed_ticks, elapsed_ms;
    unsigned char midnight;
    int is_v2 = 0, block_addressed = 0;

    if (count == 0) count = 1;
    if (count > SD_MAX_SECTORS) {
        fprintf(stderr, "sdtest: fastbench: count=%u exceeds SD_MAX_SECTORS=%u\n", count, SD_MAX_SECTORS);
        return 3;
    }

    if (sd2_init(&is_v2, &block_addressed) != 0) return 1;

    printf("sdtest: fastbench: %u sector(s) at lba=%lu in one SD_OP_READ_SECTORS (data discarded)\n", count, lba);

    t0 = bios_ticks(NULL);
    if (sd2_read_sectors(lba, count, NULL, 0) != 0) {
        fprintf(stderr, "sdtest: fastbench: SD_OP_READ_SECTORS failed\n");
        return 1;
    }
    t1 = bios_ticks(&midnight);

    elapsed_ticks = t1 - t0;
    elapsed_ms = (elapsed_ticks * 54925ul) / 1000ul;

    printf("sdtest: fastbench: %lu ticks (~%lu ms) for %u blocks, ~%lu ms/block\n",
           elapsed_ticks, elapsed_ms, count,
           count ? (elapsed_ms / count) : 0ul);
    if (midnight) {
        fprintf(stderr, "sdtest: fastbench: WARNING -- midnight rollover during run, timing above is unreliable\n");
    }

    return 0;
}

static unsigned long parse_hex_ul(const char *s) {
    unsigned long v = 0;
    sscanf(s, "%lx", &v);
    return v;
}

int main(int argc, char **argv) {
    const char *cmd = NULL;
    const char *args[4];
    int nargs = 0;
    int i;
    int rc;

    base_port = 0x378;

    for (i = 1; i < argc; i++) {
        if ((argv[i][0] == '/' || argv[i][0] == '-') &&
            (argv[i][1] == 'P' || argv[i][1] == 'p') && argv[i][2] == '=') {
            base_port = (unsigned)parse_hex_ul(argv[i] + 3);
        } else if ((argv[i][0] == '/' || argv[i][0] == '-') &&
                   (argv[i][1] == 'V' || argv[i][1] == 'v') && argv[i][2] == '\0') {
            g_verbose = 1;
        } else if (cmd == NULL) {
            cmd = argv[i];
        } else if (nargs < 4) {
            args[nargs++] = argv[i];
        }
    }

    if (cmd == NULL) {
        fprintf(stderr,
            "usage: SDTEST <init|dir|cat NAME|readsector LBA|writesector LBA -y|bench [COUNT] [LBA]|\n"
            "               fastinit|fastcheck|fastread LBA [COUNT]|fastwrite LBA -y|fastbench [COUNT] [LBA]> [/P=xxxx] [-v]\n");
        return 3;
    }

    printf("sdtest: port=0x%X\n", base_port);
    ctrl_out(CTRL_IDLE_DEFAULT);
    host_reset();

    if (strcmp(cmd, "init") == 0) {
        rc = cmd_init();
    } else if (strcmp(cmd, "dir") == 0) {
        rc = cmd_dir();
    } else if (strcmp(cmd, "cat") == 0 && nargs >= 1) {
        rc = cmd_cat(args[0]);
    } else if (strcmp(cmd, "readsector") == 0 && nargs >= 1) {
        rc = cmd_readsector(parse_hex_ul(args[0]));
    } else if (strcmp(cmd, "writesector") == 0 && nargs >= 1) {
        int confirmed = (nargs >= 2 && strcmp(args[1], "-y") == 0);
        rc = cmd_writesector(parse_hex_ul(args[0]), confirmed);
    } else if (strcmp(cmd, "bench") == 0) {
        /* Both optional: COUNT (decimal, default 57 -- matches the
         * block count dir_log.txt's DIR trace showed) and LBA (hex,
         * default 0 -- any valid sector, content is never read). */
        unsigned long count = (nargs >= 1) ? strtoul(args[0], NULL, 10) : 57ul;
        unsigned long lba   = (nargs >= 2) ? parse_hex_ul(args[1]) : 0ul;
        rc = cmd_bench(lba, (unsigned)count);
    } else if (strcmp(cmd, "fastinit") == 0) {
        rc = cmd_fastinit();
    } else if (strcmp(cmd, "fastcheck") == 0) {
        rc = cmd_fastcheck();
    } else if (strcmp(cmd, "fastread") == 0 && nargs >= 1) {
        /* LBA required (hex), COUNT optional (decimal, default 1). */
        unsigned long lba   = parse_hex_ul(args[0]);
        unsigned long count = (nargs >= 2) ? strtoul(args[1], NULL, 10) : 1ul;
        rc = cmd_fastread(lba, (unsigned)count);
    } else if (strcmp(cmd, "fastwrite") == 0 && nargs >= 1) {
        int confirmed = (nargs >= 2 && strcmp(args[1], "-y") == 0);
        rc = cmd_fastwrite(parse_hex_ul(args[0]), confirmed);
    } else if (strcmp(cmd, "fastbench") == 0) {
        /* Same defaults as `bench` -- see that branch's comment. */
        unsigned long count = (nargs >= 1) ? strtoul(args[0], NULL, 10) : 57ul;
        unsigned long lba   = (nargs >= 2) ? parse_hex_ul(args[1]) : 0ul;
        rc = cmd_fastbench(lba, (unsigned)count);
    } else {
        fprintf(stderr,
            "usage: SDTEST <init|dir|cat NAME|readsector LBA|writesector LBA -y|bench [COUNT] [LBA]|\n"
            "               fastinit|fastcheck|fastread LBA [COUNT]|fastwrite LBA -y|fastbench [COUNT] [LBA]> [/P=xxxx] [-v]\n");
        rc = 3;
    }

    /* Leave the card/link in a known state regardless of outcome. */
    spi_op_cs(0);

    return rc;
}
