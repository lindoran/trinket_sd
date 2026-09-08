/*
 * iftest.c -- DOS-side raw interface test for the RP2040 LPT trinket
 * link, built for Borland C++ 3.1 (small model). Companion to the
 * trinket_ifacetest Pico project, which runs a small command dispatcher
 * in front of the same echo-the-last-byte test harness this file always
 * talked to (see src/dispatch.h) -- every transfer this file sends has
 * to be preceded by its own dispatch command byte, selecting that
 * subsystem.
 *
 * This is a standalone diagnostic, NOT part of TRINKET.SYS/trinket.asm.
 * It talks to the parallel port directly, with no driver in between, so
 * it can be run with nothing else loaded. It duplicates (in C, not asm)
 * exactly the two handshakes documented in docs/trinket_lpt_protocol.md
 * sec 5.1/5.2/5.3 and already implemented in client_side/trinket.asm's
 * host_write_byte / host_read_byte / host_select_direction -- same
 * control-register bit meanings, same polarity, same sequencing. If this
 * works and TRINKET.SYS doesn't (or vice versa), that tells you whether
 * the bug is in the wire protocol or in the driver plumbing around it.
 * (driver/backend.c does the same dispatch-byte injection this file
 * does, transparently, on every backend_write_byte()/backend_read_byte()
 * call -- keep the two in sync the same way their handshake logic
 * already has to be kept in sync; see this file's other header notes.)
 *
 * Usage:
 *   IFTEST.EXE [byte_hex] [port_hex]
 *     byte_hex  - byte to send, hex, no prefix (default 5A)
 *     port_hex  - LPT base I/O port, hex, no prefix (default 378)
 *
 * Sequence: pulse a hardware reset on the trinket (nAuto-LF, doc sec 2 /
 * README.md's inversion table -- same pulse TRINKET.SYS's do_init() now
 * sends automatically on load, see driver/backend.c's backend_reset()),
 * then select Write, send a dispatch command byte selecting the test
 * harness, send byte_hex; select Read, send another dispatch command
 * byte, read a byte back; print both and say MATCH or MISMATCH. Exits 0
 * on match, 1 on mismatch, 2 on a handshake timeout.
 *
 * Deliberately does NOT read the BIOS Data Area for the LPT port --
 * override on the command line instead. One less untested code path.
 *
 * PORTED FROM ia16-elf-gcc (2026-09-08): this project's DOS toolchain is
 * moving to Borland C++ 3.1 across the board -- ia16-elf-gcc has no
 * viable fresh-sandbox install path (see TOOLCHAIN.md), while BC31 is
 * already vendored, already proven end-to-end (TRINKET.SYS, real
 * hardware, Wolf3D milestone) and already has a working headless
 * dosbox build script. The only change of substance from the
 * ia16-elf-gcc version: outportb()/inportb() were hand-rolled GCC
 * inline asm there (needed because ia16-elf-gcc has no built-in port
 * I/O); Borland's <dos.h> provides both natively, so the inline asm is
 * gone and there is no assembly anywhere in this file. Every other
 * line -- handshake sequencing, control-bit meanings, timing constants,
 * command-line handling -- is unchanged. This file has no far pointers
 * and does no 32-bit shifts/multiplies at all, so neither of this
 * project's two established BC31 hazards (DWORD-shift-from-a-private-
 * stack, far-pointer offset-only arithmetic -- see TOOLCHAIN.md/driver
 * notes) applies here; those are specific to TRINKET.SYS's device-
 * driver calling context and to buffers DOS itself hands in via far
 * pointers, neither of which exists in a plain .EXE that owns all its
 * own storage. Still genuinely UNTESTED against real hardware -- only
 * compiled clean under the vendored bcc via headless dosbox so far (see
 * TOOLCHAIN.md for that verification). Build and try it on real
 * hardware before trusting it.
 *
 * Build:
 *   bcc -ms -I$(INCLUDE) -L$(LIB) iftest.c
 * (see firmware/client/MAKEFILE; matches src/MAKEFILE's -ms small-model
 * convention for TRINKET.SYS itself.) Produces IFTEST.EXE (bcc names
 * the .EXE from the source basename when no -o is given -- see this
 * project's TOOLCHAIN.md for why -o is deliberately NOT used here: it
 * names the *object* file, not the executable, and silently breaks the
 * automatic link step if pointed at a .EXE name. Renamed from
 * ifacetest.c/IFACETEST.EXE during the BC31 port (2026-09-08): DOS 8.3
 * filenames can't hold "ifacetest" (9 chars, one over the limit) --
 * this also finally matches the name the ia16-elf-gcc-era Makefile's
 * IFTEST.EXE target already used, which the old source/header comments
 * never actually agreed on.
 */

#include <stdio.h>
#include <stdlib.h>
#include <dos.h>

/* ---------------------------------------------------------------------
 * Raw port I/O -- Borland's <dos.h> outportb()/inportb() are used
 * directly; no inline asm needed on this toolchain. (The ia16-elf-gcc
 * version of this file hand-rolled these with GCC inline asm because
 * that toolchain has no built-in equivalent -- see this file's header
 * comment.)
 * ------------------------------------------------------------------- */

/* ---------------------------------------------------------------------
 * Control register bit assignments -- doc sec 2's host-side inversion
 * table, identical to trinket.asm's CTRL_* constants. Keep these two
 * files in sync if either changes; they encode the same hardware.
 * ------------------------------------------------------------------- */

#define CTRL_STROBE_SELECT_LOW   0x01u
#define CTRL_STROBE_SELECT_HIGH  0x00u

#define CTRL_AUTOLF_IDLE         0x02u   /* idle/released -- physical pin LOW */
#define CTRL_AUTOLF_ASSERT       0x00u   /* reset asserted -- physical pin HIGH */

#define CTRL_INIT_RAISED         0x04u
#define CTRL_INIT_DROPPED        0x00u

#define CTRL_SELECT_WRITE        0x00u
#define CTRL_SELECT_READ         0x08u

#define CTRL_IDLE_DEFAULT  (CTRL_AUTOLF_IDLE | CTRL_INIT_DROPPED | CTRL_SELECT_WRITE)

#define REG_DATA     0   /* offset from base: write, D0-D7 */
#define REG_STATUS   1   /* offset from base: read, bits 7:4 nibble, bit3 Error */
#define REG_CONTROL  2   /* offset from base: write-only -- always via shadow */

/* Device-side dispatch command byte -- must match
 * trinket_ifacetest/src/dispatch.h's DISPATCH_CMD_TEST_HARNESS exactly.
 * Two separate translation units on two separate toolchains (this one,
 * Borland C++ 3.1; the trinket's, arm-none-eabi-gcc via the Pico SDK),
 * so there's no shared header to enforce this -- keep the numeric value
 * in sync by hand if either side's dispatch command list changes. Same
 * constant driver/backend.c's DISPATCH_CMD_TEST_HARNESS defines
 * independently for the same reason. */
#define DISPATCH_CMD_TEST_HARNESS  0x01u

/* Not a measured value -- doc sec 9 open item 1 (timing test) hasn't
 * been run. Generous placeholder to avoid a truly infinite hang. */
#define POLL_TIMEOUT_ITERS  60000UL

static unsigned base_port;
static unsigned char ctrl_shadow = CTRL_IDLE_DEFAULT;

static void ctrl_out(unsigned char value) {
    ctrl_shadow = value;
    outportb(base_port + REG_CONTROL, value);
}

/* Settle delay for control lines that go through the level shifter (U2)
 * -- nStrobe, nAuto-LF, nInit, nSelect-In. Port 0x80 is the classic
 * "diagnostic POST port" trick for a short, timing-source-independent
 * ISA bus-cycle delay; reading it is otherwise harmless. Not a measured
 * value (doc sec 9 open item 1, timing test, still hasn't been run) --
 * this exists because raising Init immediately after changing Strobe,
 * with literally zero delay between the two OUTs, looks like the
 * likely cause of the low-nibble-only flakiness: Strobe hasn't
 * necessarily finished its transition through the level shifter by the
 * time Init arrives on a fast host. Cycle 2's Strobe flip gets this
 * settle time "for free" from wait_error_low()'s poll loop before it;
 * cycle 1's doesn't, which is why only the low nibble was affected. */
static void io_delay(void) {
    int i;
    for (i = 0; i < 4; i++) {
        inportb(0x80);
    }
}

/* Reset pulse hold duration -- same unmeasured-but-generous placeholder
 * as driver/backend.c's reset_hold_delay(), same rationale: has to cover
 * the trinket noticing the IRQ, unwinding out of its current blocking
 * wait, and parking its outputs, not just one level-shifter transition.
 * Keep this in sync with backend.c's copy if either changes. */
static void reset_hold_delay(void) {
    int i;
    for (i = 0; i < 200; i++) {
        io_delay();
    }
}

/* Returns 0 on success, 1 on timeout. */
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

/* doc sec 5.3 -- call once, before the first Init raise of a transfer. */
static void host_select_direction(int want_write) {
    unsigned char v = (unsigned char)(ctrl_shadow & ~CTRL_SELECT_READ);
    v = (unsigned char)(v | (want_write ? CTRL_SELECT_WRITE : CTRL_SELECT_READ));
    ctrl_out(v);
    io_delay();   /* same level-shifter settle concern as Strobe */
}

/* Pulse the nAuto-LF hardware reset line -- direct port-I/O port of
 * driver/backend.c's backend_reset(), same assert/hold/release
 * sequence and same rationale for keeping them in sync. Called
 * unconditionally at the start of main() (see below) so this
 * standalone diagnostic gets the same known-starting-state guarantee
 * TRINKET.SYS's do_init() now gives driver-based callers, without
 * requiring the driver to be loaded at all. */
static void host_reset(void) {
    ctrl_out((unsigned char)(CTRL_AUTOLF_ASSERT | CTRL_INIT_DROPPED | CTRL_SELECT_WRITE));
    reset_hold_delay();
    ctrl_out(CTRL_IDLE_DEFAULT);
    io_delay();
}

/* One Init/Error byte-handshake cycle (doc sec 5.2), no dispatch
 * wrapping -- shared by host_write_byte()'s dispatch-byte send and its
 * payload-byte send, and by host_read_byte()'s dispatch-byte send, so
 * this sequence exists exactly once instead of three times. Direction
 * must already be Write when this is called -- it doesn't touch
 * nSelect-In itself. */
static int raw_send_byte(unsigned char byte) {
    outportb(base_port + REG_DATA, byte);

    ctrl_out((unsigned char)(ctrl_shadow | CTRL_INIT_RAISED));
    if (wait_error_high()) return 1;

    ctrl_out((unsigned char)(ctrl_shadow & ~CTRL_INIT_RAISED));
    if (wait_error_low()) return 1;

    return 0;
}

/* doc sec 5.2 -- one full Init/Error cycle. Returns 0 on success.
 * Sends the dispatch command byte first -- see this file's header
 * comment and src/dispatch.h. Direction must already be Write (caller's
 * responsibility, same contract as before dispatch existed). */
static int host_write_byte(unsigned char byte) {
    if (raw_send_byte(DISPATCH_CMD_TEST_HARNESS)) return 1;

    return raw_send_byte(byte);
}

/* doc sec 5.1 -- two Init/Error cycles, low nibble then high. Returns 0
 * on success with *out set; 1 on timeout. Sends the dispatch command
 * byte first, same as host_write_byte() -- but it always flows
 * host->device regardless of which direction the *payload* transfer
 * that follows will use, so direction is forced to Write for just that
 * one byte, then switched back to Read for the nibble handshake below.
 * host_select_direction() unconditionally sets both bits it touches, so
 * nothing needs to be saved/restored -- the Read the caller originally
 * selected before calling this is just re-applied. */
static int host_read_byte(unsigned char *out) {
    unsigned char status, lo, hi;

    host_select_direction(1 /* write, for the dispatch byte only */);
    if (raw_send_byte(DISPATCH_CMD_TEST_HARNESS)) return 1;
    host_select_direction(0 /* back to read, for the payload below */);

    /* cycle 1: low nibble */
    ctrl_out((unsigned char)((ctrl_shadow & ~0x01u) | CTRL_STROBE_SELECT_LOW));
    io_delay();   /* let Strobe actually settle before Init arrives */
    ctrl_out((unsigned char)(ctrl_shadow | CTRL_INIT_RAISED));
    if (wait_error_high()) return 1;
    status = inportb(base_port + REG_STATUS);
    lo = (unsigned char)(status & 0xF0u);
    ctrl_out((unsigned char)(ctrl_shadow & ~CTRL_INIT_RAISED));
    if (wait_error_low()) return 1;

    /* cycle 2: high nibble */
    ctrl_out((unsigned char)((ctrl_shadow & ~0x01u) | CTRL_STROBE_SELECT_HIGH));
    io_delay();   /* same settle margin, for symmetry/safety */
    ctrl_out((unsigned char)(ctrl_shadow | CTRL_INIT_RAISED));
    if (wait_error_high()) return 1;
    status = inportb(base_port + REG_STATUS);
    hi = (unsigned char)(status & 0xF0u);
    ctrl_out((unsigned char)(ctrl_shadow & ~CTRL_INIT_RAISED));
    if (wait_error_low()) return 1;

    /* raw reconstruction only -- no 0x88 correction here, that already
     * happened on the trinket side (doc sec 7 contract). */
    *out = (unsigned char)((lo >> 4) | hi);
    return 0;
}

static unsigned parse_hex(const char *s, unsigned fallback) {
    unsigned v;
    if (s == NULL || s[0] == '\0') {
        return fallback;
    }
    if (sscanf(s, "%x", &v) != 1) {
        return fallback;
    }
    return v;
}

int main(int argc, char **argv) {
    unsigned char send_byte;
    unsigned char recv_byte;

    send_byte = (unsigned char)parse_hex(argc > 1 ? argv[1] : NULL, 0x5A);
    base_port = parse_hex(argc > 2 ? argv[2] : NULL, 0x378);

    printf("ifacetest: port=0x%X send=0x%02X\n", base_port, send_byte);

    /* Known-idle starting point for this shadow -- do not assume
     * anything about the control register's power-on state. */
    ctrl_out(CTRL_IDLE_DEFAULT);

    printf("reset: pulsing nAuto-LF...\n");
    host_reset();
    printf("reset: done\n");

    printf("write: select direction...\n");
    host_select_direction(1);

    printf("write: sending byte...\n");
    if (host_write_byte(send_byte)) {
        printf("write: TIMEOUT waiting for trinket ack\n");
        return 2;
    }
    printf("write: done\n");

    printf("read: select direction...\n");
    host_select_direction(0);
    /* Redundant as of dispatch: host_read_byte() below re-selects Read
     * itself, right after sending its own dispatch byte (which forces
     * Write first). Left here anyway -- matches the original pre-
     * dispatch call shape/log output, and costs nothing but one extra
     * ctrl_out(). */

    printf("read: waiting for byte...\n");
    if (host_read_byte(&recv_byte)) {
        printf("read: TIMEOUT waiting for trinket data\n");
        return 2;
    }

    printf("sent=0x%02X received=0x%02X -- %s\n",
           send_byte, recv_byte,
           (send_byte == recv_byte) ? "MATCH" : "MISMATCH");

    return (send_byte == recv_byte) ? 0 : 1;
}
