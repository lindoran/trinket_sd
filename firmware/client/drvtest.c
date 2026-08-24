/*
 * drvtest.c -- exercises TRNKT$ through normal DOS file I/O, i.e. through
 * the actual driver plumbing (header/strategy/interrupt/dispatch), not
 * raw port I/O like client/ifacetest.c.
 *
 * This is the counterpart ifacetest.c's own header comment anticipates:
 * "If this works and TRINKET.SYS doesn't (or vice versa), that tells you
 * whether the bug is in the wire protocol or in the driver plumbing
 * around it." Run ifacetest.c FIRST. Only once that passes does a failure
 * here actually isolate to entry.S/driver.c, rather than leaving open the
 * question of whether the wire protocol itself is still suspect.
 *
 * Usage:
 *   DRVTEST.EXE [byte_hex]
 *     byte_hex - byte to send, hex, no prefix (default 5A)
 *   DRVTEST.EXE -r
 *     Send an on-demand reset through the driver (DOS generic IOCTL,
 *     AH=44h function 03h "write to control channel") instead of
 *     running the write/read test. Exercises driver.c's do_ioctl_reset()
 *     -- the same backend_reset() pulse do_init() sends automatically
 *     on load, but callable any time TRNKT$ is open, e.g. to recover a
 *     wedged transfer without unloading/reloading TRINKET.SYS.
 *
 * Requires TRINKET.SYS loaded via CONFIG.SYS first:
 *   DEVICE=TRINKET.SYS [/P=xxxx]
 *
 * Sequence (default mode): open TRNKT$, write byte_hex, read a byte
 * back, print both, say MATCH or MISMATCH. Exit 0 on match, 1 on
 * mismatch, 2 on an open or I/O failure (distinct from ifacetest.c's
 * "2 = handshake timeout" -- here a timeout surfaces as an I/O error
 * from fwrite/fread instead, since the driver reports it via the DOS
 * error path, not directly). -r mode: exit 0 on success, 2 on an open
 * or IOCTL failure.
 *
 * UNTESTED: same status as everything else in this pass -- written by
 * inspection, not run through ia16-elf-gcc or real DOS. Build both
 * ifacetest.c and this and compare their behavior on real hardware
 * before trusting either. The -r path has an extra untested assumption
 * of its own: it calls fileno() to get the raw DOS handle stdio opened
 * for TRNKT$, which needs to actually exist and do the obvious thing on
 * your ia16-elf-gcc/libc version -- check that first if -r doesn't
 * build.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

/* DOS AH=44h function 03h -- "write to control channel" (generic IOCTL
 * output). Returns 0 on success, 1 if DOS set the carry flag (error).
 * Standard GCC x86 inline-asm idiom, same style as client/ifacetest.c's
 * outportb/inportb: fixed hardware registers pinned to the exact 8086
 * ABI the interrupt expects (AX=function, BX=handle, CX=count,
 * DX=buffer offset). Reads the carry flag via jnc/local-label branching
 * rather than a "=@ccc" flag-output constraint -- deliberately the more
 * conservative, older idiom, since we can't confirm flag-output operand
 * support on this specific ia16-elf-gcc version any more than
 * ifacetest.c's header comment could confirm its own inline asm
 * beforehand. "1:"/"2:" are GCC's numeric local labels, safe to reuse
 * even if this expands more than once in the same translation unit
 * (GCC renames them per-instance automatically). DX=buffer assumes a
 * near (default-model) pointer is valid as given, same assumption
 * ifacetest.c's header comment makes for that file's build -- true here
 * too, buf is a local array, not a far object.
 */
static int dos_ioctl_write(int handle, const void *buf, unsigned count) {
    unsigned result;
    __asm__ volatile (
        "int $0x21\n\t"
        "jnc 1f\n\t"
        "movw $1, %0\n\t"
        "jmp 2f\n\t"
        "1:\n\t"
        "movw $0, %0\n\t"
        "2:"
        : "=r"(result)
        : "a"(0x4403), "b"(handle), "c"(count), "d"(buf)
        : "cc", "memory"
    );
    return (int)result;
}

/* Payload content doesn't matter -- driver.c's do_ioctl_reset() treats
 * any control-channel write as "reset", regardless of what's in it (see
 * driver/device.h's CMD_IOCTL_OUTPUT comment). One byte is the minimum
 * DOS will actually transfer. */
static int do_reset(void) {
    FILE *dev;
    int handle;
    unsigned char payload = 0;

    printf("drvtest: opening TRNKT$ for reset...\n");
    dev = fopen("TRNKT$", "r+b");
    if (dev == NULL) {
        printf("drvtest: could not open TRNKT$ -- is TRINKET.SYS loaded?\n");
        return 2;
    }

    handle = fileno(dev);

    printf("drvtest: sending IOCTL reset...\n");
    if (dos_ioctl_write(handle, &payload, 1)) {
        printf("drvtest: IOCTL reset failed (DOS returned an error)\n");
        fclose(dev);
        return 2;
    }

    fclose(dev);
    printf("drvtest: reset done\n");
    return 0;
}

int main(int argc, char **argv) {
    unsigned char send_byte;
    unsigned char recv_byte;
    FILE *dev;
    size_t n;

    if (argc > 1 && strcmp(argv[1], "-r") == 0) {
        return do_reset();
    }

    send_byte = (unsigned char)parse_hex(argc > 1 ? argv[1] : NULL, 0x5A);

    printf("drvtest: opening TRNKT$, send=0x%02X\n", send_byte);

    dev = fopen("TRNKT$", "r+b");
    if (dev == NULL) {
        printf("drvtest: could not open TRNKT$ -- is TRINKET.SYS loaded?\n");
        return 2;
    }

    /* TRNKT$ is a device, not a seekable file: disable stdio buffering so
     * each fwrite/fread turns directly into one OUTPUT/INPUT request to
     * the driver, with no read-after-write reordering surprises from a
     * buffered stream that (per C's own rules) would otherwise need an
     * explicit flush/seek between a write and a following read anyway. */
    setvbuf(dev, NULL, _IONBF, 0);

    printf("drvtest: writing byte...\n");
    n = fwrite(&send_byte, 1, 1, dev);
    if (n != 1) {
        printf("drvtest: write failed/short (wrote %u bytes)\n", (unsigned)n);
        fclose(dev);
        return 2;
    }
    printf("drvtest: write done\n");

    printf("drvtest: reading byte...\n");
    n = fread(&recv_byte, 1, 1, dev);
    if (n != 1) {
        printf("drvtest: read failed/short (got %u bytes)\n", (unsigned)n);
        fclose(dev);
        return 2;
    }

    fclose(dev);

    printf("sent=0x%02X received=0x%02X -- %s\n",
           send_byte, recv_byte,
           (send_byte == recv_byte) ? "MATCH" : "MISMATCH");

    return (send_byte == recv_byte) ? 0 : 1;
}
