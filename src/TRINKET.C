/*-------------------------------------------------------------------------/
/  trinket.c -- diskio.h backend for the trinket SD-over-parallel-port
/  interface. Drop-in replacement for sdpp-main's SDMM.C: same public
/  contract (disk_initialize/disk_status/disk_result/disk_check_media/
/  disk_read/disk_write/disk_ioctl/setportbase, plus the sd_card_check/
/  portbase globals that DRIVER.C's parse_options() already pokes), so
/  SD.C, DRIVER.C and HEADER.ASM integrate cleanly.
/
/  Where SDMM.C bit-bangs SPI directly on four parallel-port data lines,
/  this module speaks to a trinket (RP2040 parallel-port-to-SPI bridge)
/  using its high-level SD subsystem protocol (DISPATCH_CMD_SD, 0x03):
/
/    Section 1 -- wire-level Init/Error byte + nibble handshakes.
/                 Ported from trinket_ifacetest's driver/backend.c and
/                 sdtest.c. Uses Borland/Turbo C's outp()/inp() (<conio.h>).
/                 Includes raw_recv_bulk() for high-speed streaming nibble
/                 reads without per-byte function call overhead.
/
/    Section 2 -- DISPATCH_CMD_SD high-level subsystem (subsys_sd.h).
/                 Moves the entire SD protocol (CMD0/CMD8/ACMD41/CMD58/
/                 CMD16/CMD17/CMD24/CMD13, R1/R7 parsing, token wait) onto
/                 the RP2040 firmware. The host issues high-level requests:
/                   - SD_OP_INIT (0x01): RP2040 runs full card bring-up.
/                   - SD_OP_READ_SECTORS (0x02): RP2040 reads N sectors via
/                     SPI and streams payload over LPT in one dispatch cycle.
/                   - SD_OP_WRITE_SECTORS (0x03): host streams N sectors,
/                     RP2040 writes via SPI in one dispatch cycle.
/                   - SD_OP_CHECK_MEDIA (0x04): fast 1-byte status probe
/                     (CMD13) to verify card presence with minimal bus traffic.
/
/    Section 3 -- Far Pointer Normalization (advance_far_ptr).
/                 Normalizes segment:offset on every block increment to
/                 guarantee offset < 16, eliminating 64KB segment wrap bugs
/                 during multi-sector transfers using 16-bit-only arithmetic
/                 (no 32-bit cs.lib helper dependencies).
/
/    Section 4 -- diskio.h public interface.
/                 Provides disk_initialize(), disk_status(), disk_result(),
/                 disk_check_media(), disk_read(), disk_write(), disk_ioctl(),
/                 and setportbase().
/-------------------------------------------------------------------------*/

#include <conio.h>      /* inp/outp */
#include <dos.h>        /* FP_SEG/FP_OFF/MK_FP for advance_far_ptr() */

#include "diskio.h"     /* disk_* contract this module implements */
#include "cprint.h"     /* cdprintf, for init-time diagnostics */

/* ===================================================================
 * Section 0: globals DRIVER.C's parse_options() expects to exist.
 * =================================================================== */

BYTE sd_card_check = 0;   /* /k -- accepted, not implemented */
BYTE portbase = 2;        /* /b -- index into port_bases[], default 0x378 */

static WORD port_bases[5] = {0x3BCu, 0x378u, 0x278u, 0x3E8u, 0x2E8u};
static WORD base_port = 0x378u;

/* ===================================================================
 * Section 1: wire-level Init/Error byte + nibble handshakes.
 * =================================================================== */

#define outportbyte(a,b)  outp(a,b)
#define inportbyte(a)     inp(a)

#define CTRL_STROBE_SELECT_LOW   0x01u
#define CTRL_STROBE_SELECT_HIGH  0x00u

#define CTRL_AUTOLF_IDLE          0x02u   /* idle/released -- pin LOW */
#define CTRL_AUTOLF_ASSERT        0x00u   /* reset asserted -- pin HIGH */

#define CTRL_INIT_RAISED         0x04u
#define CTRL_INIT_DROPPED        0x00u

#define CTRL_SELECT_WRITE        0x00u
#define CTRL_SELECT_READ         0x08u

#define CTRL_IDLE_DEFAULT  (CTRL_AUTOLF_IDLE | CTRL_INIT_DROPPED | CTRL_SELECT_WRITE)

#define REG_DATA     0
#define REG_STATUS   1
#define REG_CONTROL  2

#define POLL_TIMEOUT_ITERS  60000u
#define IO_DELAY_ITERS      4

static BYTE ctrl_shadow = CTRL_IDLE_DEFAULT;

static void ctrl_out(BYTE value)
{
   ctrl_shadow = value;
   outportbyte(base_port + REG_CONTROL, value);
}

/* Port 0x80 "diagnostic POST port" read -- timing-source-independent
 * ISA bus delay, same trick backend.c/sdtest.c use. */
static void io_delay(void)
{
   int i;
   for (i = 0; i < IO_DELAY_ITERS; i++)
      inportbyte(0x80);
}

static void reset_hold_delay(void)
{
   int i;
   for (i = 0; i < 200; i++)
      io_delay();
}

static int wait_error_high(void)
{
   UINT i;
   for (i = 0; i < POLL_TIMEOUT_ITERS; i++)
      if (inportbyte(base_port + REG_STATUS) & 0x08u) return 0;
   return 1;
}

static int wait_error_low(void)
{
   UINT i;
   for (i = 0; i < POLL_TIMEOUT_ITERS; i++)
      if (!(inportbyte(base_port + REG_STATUS) & 0x08u)) return 0;
   return 1;
}

static void host_select_direction(int want_write)
{
   BYTE v = (BYTE)(ctrl_shadow & ~CTRL_SELECT_READ);
   v = (BYTE)(v | (want_write ? CTRL_SELECT_WRITE : CTRL_SELECT_READ));
   ctrl_out(v);
   io_delay();
}

/* backend_reset() equivalent -- assert nAuto-LF, hold, release. */
static void host_reset(void)
{
   ctrl_out((BYTE)(CTRL_AUTOLF_ASSERT | CTRL_INIT_DROPPED | CTRL_SELECT_WRITE));
   reset_hold_delay();
   ctrl_out(CTRL_IDLE_DEFAULT);
   io_delay();
}

/* One Init/Error byte-handshake cycle, host->device, direction already Write.
 * Returns 0 on success, 1 on timeout. */
static int raw_send_byte(BYTE b)
{
   outportbyte(base_port + REG_DATA, b);

   ctrl_out((BYTE)(ctrl_shadow | CTRL_INIT_RAISED));
   if (wait_error_high()) return 1;

   ctrl_out((BYTE)(ctrl_shadow & ~CTRL_INIT_RAISED));
   if (wait_error_low()) return 1;

   return 0;
}

/* One byte, two-cycle nibble handshake, device->host, direction already Read. */
static int raw_recv_byte(BYTE *out)
{
   BYTE status, lo, hi;

   ctrl_out((BYTE)((ctrl_shadow & ~0x01u) | CTRL_STROBE_SELECT_LOW));
   io_delay();
   ctrl_out((BYTE)(ctrl_shadow | CTRL_INIT_RAISED));
   if (wait_error_high()) return 1;
   status = inportbyte(base_port + REG_STATUS);
   lo = (BYTE)(status & 0xF0u);
   ctrl_out((BYTE)(ctrl_shadow & ~CTRL_INIT_RAISED));
   if (wait_error_low()) return 1;

   ctrl_out((BYTE)((ctrl_shadow & ~0x01u) | CTRL_STROBE_SELECT_HIGH));
   io_delay();
   ctrl_out((BYTE)(ctrl_shadow | CTRL_INIT_RAISED));
   if (wait_error_high()) return 1;
   status = inportbyte(base_port + REG_STATUS);
   hi = (BYTE)(status & 0xF0u);
   ctrl_out((BYTE)(ctrl_shadow & ~CTRL_INIT_RAISED));
   if (wait_error_low()) return 1;

   *out = (BYTE)((lo >> 4) | hi);
   return 0;
}

/* Bulk nibble read -- reads `count` bytes via the exact same per-byte
 * protocol as raw_recv_byte() in ONE tight loop without per-byte function
 * call overhead (ENHANCEMENTS.md point 4).
 * buf is a far pointer (BYTE DOSFAR *), or 0 to discard.
 *
 * FIX (see this project's own established point-5 far-pointer-wrap
 * bug and its fix elsewhere in this file): the original version of
 * this function wrote via `buf[i]`, which for a `far` pointer compiles
 * to a plain 16-bit offset add with no segment carry (confirmed via
 * -S: `les bx,[buf]; add bx,si; mov es:[bx],al` -- BX is the offset
 * only, ES never adjusted). A 512-byte sector read into a caller
 * buffer positioned within the last 511 bytes of its 64K segment would
 * silently wrap and write to the wrong physical address -- this is
 * reachable on every ordinary single-sector disk_read(), not just a
 * multi-sector edge case, since it depends only on where DOS happened
 * to place the destination buffer. Walking a local far pointer forward
 * with advance_far_ptr(p, 1) instead of indexing buf[i] fixes this the
 * same way every other far-pointer walk in this file already does. */
static BYTE DOSFAR *advance_far_ptr(BYTE DOSFAR *p, WORD add_bytes);

static int raw_recv_bulk(BYTE DOSFAR *buf, UINT count)
{
   BYTE status, lo, hi, val;
   UINT i;
   BYTE DOSFAR *p = buf;

   for (i = 0; i < count; i++) {
      ctrl_out((BYTE)((ctrl_shadow & ~0x01u) | CTRL_STROBE_SELECT_LOW));
      io_delay();
      ctrl_out((BYTE)(ctrl_shadow | CTRL_INIT_RAISED));
      if (wait_error_high()) return 1;
      status = inportbyte(base_port + REG_STATUS);
      lo = (BYTE)(status & 0xF0u);
      ctrl_out((BYTE)(ctrl_shadow & ~CTRL_INIT_RAISED));
      if (wait_error_low()) return 1;

      ctrl_out((BYTE)((ctrl_shadow & ~0x01u) | CTRL_STROBE_SELECT_HIGH));
      io_delay();
      ctrl_out((BYTE)(ctrl_shadow | CTRL_INIT_RAISED));
      if (wait_error_high()) return 1;
      status = inportbyte(base_port + REG_STATUS);
      hi = (BYTE)(status & 0xF0u);
      ctrl_out((BYTE)(ctrl_shadow & ~CTRL_INIT_RAISED));
      if (wait_error_low()) return 1;

      val = (BYTE)((lo >> 4) | hi);
      if (buf != 0) {
         *p = val;
         p = advance_far_ptr(p, 1);
      }
   }
   return 0;
}

/* ===================================================================
 * Section 2: DISPATCH_CMD_SD high-level subsystem (subsys_sd.h framing).
 * Ported from trinket firmware subsys_sd.h and client/sdtest.c Section 2b.
 * =================================================================== */

#define DISPATCH_CMD_SD  0x03u

#define SD_OP_INIT           0x01u
#define SD_OP_READ_SECTORS   0x02u
#define SD_OP_WRITE_SECTORS  0x03u
#define SD_OP_CHECK_MEDIA    0x04u

#define SD_STATUS_OK     0x00u
#define SD_STATUS_ERROR  0x01u

#define SD_MAX_SECTORS  64u

static int g_is_v2 = 0;
static int g_block_addressed = 0;

/* Forward declaration for Section 3 far pointer normalizer */
static BYTE DOSFAR *advance_far_ptr(BYTE DOSFAR *p, WORD add_bytes);

/* Fetches the result of the most recently completed SD_OP_*.
 * Response format: status(1), is_v2(1), block_addressed(1), len_lo(1), len_hi(1),
 * followed by len data bytes. Truncates into buf/bufcap if requested, draining
 * any excess bytes off the wire to keep the protocol in lockstep.
 *
 * NOTE: The Pico dispatcher expects DISPATCH_CMD_SD to be sent in Write direction
 * before switching to Read direction for the response phase. */
static int sd2_get_result(BYTE *out_status, int *out_is_v2,
   int *out_block_addressed, BYTE DOSFAR *buf, UINT bufcap, UINT *out_len)
{
   BYTE hdr[5];
   BYTE status, is_v2, block_addressed, len_lo, len_hi;
   UINT len, i, s;
   BYTE DOSFAR *p;

   host_select_direction(1);
   if (raw_send_byte(DISPATCH_CMD_SD)) return 1;
   host_select_direction(0);

   for (i = 0; i < 5; i++) {
      if (raw_recv_byte(&hdr[i])) return 1;
   }
   status = hdr[0];
   is_v2 = hdr[1];
   block_addressed = hdr[2];
   len_lo = hdr[3];
   len_hi = hdr[4];
   len = (UINT)len_lo | ((UINT)len_hi << 8);

   if (buf == 0 || bufcap == 0) {
      if (len > 0) {
         if (raw_recv_bulk(0, len)) return 1;
      }
   } else {
      UINT to_read = (bufcap < len) ? bufcap : len;
      UINT full_sectors = to_read / 512u;
      UINT rem = to_read % 512u;

      p = buf;
      for (s = 0; s < full_sectors; s++) {
         if (raw_recv_bulk(p, 512)) return 1;
         p = advance_far_ptr(p, 512);
      }
      if (rem > 0) {
         if (raw_recv_bulk(p, rem)) return 1;
      }
      if (len > to_read) {
         if (raw_recv_bulk(0, len - to_read)) return 1;
      }
   }

   *out_status = status;
   if (out_is_v2 != 0) *out_is_v2 = is_v2 ? 1 : 0;
   if (out_block_addressed != 0) *out_block_addressed = block_addressed ? 1 : 0;
   if (out_len != 0) *out_len = len;
   return 0;
}

/* SD_OP_INIT -- RP2040 runs the whole SPI init sequence on-chip. */
static int sd2_init(int *out_is_v2, int *out_block_addressed)
{
   BYTE status;

   host_select_direction(1);
   if (raw_send_byte(DISPATCH_CMD_SD)) return 1;
   if (raw_send_byte(SD_OP_INIT)) return 1;

   if (sd2_get_result(&status, out_is_v2, out_block_addressed, 0, 0, 0)) return 1;
   return (status == SD_STATUS_OK) ? 0 : -1;
}

/* SD_OP_READ_SECTORS -- reads `count` consecutive sectors starting at
 * `lba` in ONE dispatch round trip directly into `buf`. */
static int sd2_read_sectors(DWORD lba, UINT count, BYTE DOSFAR *buf)
{
   BYTE status;

   if (count == 0 || count > SD_MAX_SECTORS) return 1;

   host_select_direction(1);
   if (raw_send_byte(DISPATCH_CMD_SD)) return 1;
   if (raw_send_byte(SD_OP_READ_SECTORS)) return 1;
   if (raw_send_byte(((BYTE *)&lba)[0])) return 1;
   if (raw_send_byte(((BYTE *)&lba)[1])) return 1;
   if (raw_send_byte(((BYTE *)&lba)[2])) return 1;
   if (raw_send_byte(((BYTE *)&lba)[3])) return 1;
   if (raw_send_byte((BYTE)count)) return 1;

   if (sd2_get_result(&status, 0, 0, buf, count * 512u, 0)) return 1;
   return (status == SD_STATUS_OK) ? 0 : -1;
}

/* SD_OP_WRITE_SECTORS -- writes `count` consecutive sectors starting at
 * `lba` from `buf` in ONE dispatch round trip.
 *
 * FIX: same far-pointer-wrap bug as raw_recv_bulk() above, same fix --
 * the original `p[i]` here has the identical `add bx,si`-with-no-
 * segment-carry problem, just reading from the wrong address instead
 * of writing to it. Walks p with advance_far_ptr(p, 1) per byte now,
 * not per sector. */
static int sd2_write_sectors(DWORD lba, UINT count, const BYTE DOSFAR *buf)
{
   BYTE status;
   const BYTE DOSFAR *p;
   UINT s, i;

   if (count == 0 || count > SD_MAX_SECTORS) return 1;

   host_select_direction(1);
   if (raw_send_byte(DISPATCH_CMD_SD)) return 1;
   if (raw_send_byte(SD_OP_WRITE_SECTORS)) return 1;
   if (raw_send_byte(((BYTE *)&lba)[0])) return 1;
   if (raw_send_byte(((BYTE *)&lba)[1])) return 1;
   if (raw_send_byte(((BYTE *)&lba)[2])) return 1;
   if (raw_send_byte(((BYTE *)&lba)[3])) return 1;
   if (raw_send_byte((BYTE)count)) return 1;

   p = buf;
   for (s = 0; s < count; s++) {
      for (i = 0; i < 512; i++) {
         if (raw_send_byte(*p)) return 1;
         p = (const BYTE DOSFAR *) advance_far_ptr((BYTE DOSFAR *) p, 1);
      }
   }

   if (sd2_get_result(&status, 0, 0, 0, 0, 0)) return 1;
   return (status == SD_STATUS_OK) ? 0 : -1;
}

/* SD_OP_CHECK_MEDIA -- fast 1-byte status probe (CMD13 on RP2040). */
static int sd2_check_media(void)
{
   BYTE status;

   host_select_direction(1);
   if (raw_send_byte(DISPATCH_CMD_SD)) return 1;
   if (raw_send_byte(SD_OP_CHECK_MEDIA)) return 1;

   if (sd2_get_result(&status, 0, 0, 0, 0, 0)) return 1;
   return (status == SD_STATUS_OK) ? 0 : -1;
}

/* ===================================================================
 * Section 3: Far Pointer Normalization (advance_far_ptr).
 *
 * Normalizes segment:offset arithmetic so that the offset is always
 * kept in the range [0..15], carrying excess bytes into the segment
 * register. This completely prevents 64KB offset wraparound during
 * multi-sector transfers.
 *
 * Uses only 16-bit WORD operations (shifts, adds, masks) so no 32-bit
 * runtime helper functions from cs.lib (e.g. N_LXLSH@ / N_LXURSH@) are
 * generated in the small-model driver.
 * =================================================================== */

static BYTE DOSFAR *advance_far_ptr(BYTE DOSFAR *p, WORD add_bytes)
{
   WORD seg = FP_SEG(p);
   WORD off = FP_OFF(p);
   seg += (off >> 4);
   off = (off & 0x0F) + add_bytes;
   seg += (off >> 4);
   off &= 0x0F;
   return (BYTE DOSFAR *) MK_FP(seg, off);
}

/* ===================================================================
 * Section 4: diskio.h public interface.
 * =================================================================== */

static DSTATUS Stat = STA_NOINIT;

void setportbase(BYTE val)
{
   if ((val >= 1) && (val <= (sizeof(port_bases) / sizeof(port_bases[0]))))
      base_port = port_bases[val - 1];
   ctrl_out(CTRL_IDLE_DEFAULT);
}

DSTATUS disk_status(BYTE drv)
{
   if (drv) return STA_NOINIT;
   return Stat;
}

DRESULT disk_result(BYTE drv)
{
   if (drv) return RES_NOTRDY;
   return (Stat & STA_NOINIT) ? RES_NOTRDY : RES_OK;
}

/* Fast firmware-side media-presence check (SD_OP_CHECK_MEDIA) */
DRESULT disk_check_media(BYTE drv)
{
   if (drv) return RES_NOTRDY;
   if (Stat & STA_NOINIT) return RES_NOTRDY;
   if (sd2_check_media() != 0) return RES_NOTRDY;
   return RES_OK;
}

DSTATUS disk_initialize(BYTE drv)
{
   int is_v2 = 0, block_addressed = 0;

   setportbase(portbase);

   if (drv) return STA_NOINIT;

   host_reset();          /* known device state on every (re)init */

   if (sd2_init(&is_v2, &block_addressed) != 0) {
      cdprintf("TRINKET: SD_OP_INIT failed\n");
      Stat = STA_NOINIT;
      return Stat;
   }

   g_is_v2 = is_v2;
   g_block_addressed = block_addressed;

   cdprintf("TRINKET: card ready (%s, %s-addressed)\n",
      is_v2 ? "SDv2" : "SDv1/MMC",
      block_addressed ? "block" : "byte");

   Stat = 0;
   return Stat;
}

DRESULT disk_read(BYTE drv, BYTE DOSFAR *buff, DWORD sector, UINT count)
{
   BYTE DOSFAR *p = buff;
   DRESULT dr = disk_result(drv);
   if (dr != RES_OK) return dr;

   while (count > 0) {
      UINT chunk = (count > SD_MAX_SECTORS) ? (UINT)SD_MAX_SECTORS : count;
      UINT i;
      if (sd2_read_sectors(sector, chunk, p) != 0) return RES_ERROR;
      sector += chunk;
      for (i = 0; i < chunk; i++) {
         p = advance_far_ptr(p, 512);
      }
      count -= chunk;
   }
   return RES_OK;
}

DRESULT disk_write(BYTE drv, const BYTE DOSFAR *buff, DWORD sector, UINT count)
{
   const BYTE DOSFAR *p = buff;
   DRESULT dr = disk_result(drv);
   if (dr != RES_OK) return dr;

   while (count > 0) {
      UINT chunk = (count > SD_MAX_SECTORS) ? (UINT)SD_MAX_SECTORS : count;
      UINT i;
      if (sd2_write_sectors(sector, chunk, p) != 0) return RES_ERROR;
      sector += chunk;
      for (i = 0; i < chunk; i++) {
         p = (const BYTE DOSFAR *) advance_far_ptr((BYTE DOSFAR *) p, 512);
      }
      count -= chunk;
   }
   return RES_OK;
}

/* Not exercised by SD.C/DRIVER.C -- kept so diskio.h compiles and links cleanly. */
DRESULT disk_ioctl(BYTE drv, BYTE cmd, void DOSFAR *buff)
{
   (void)drv; (void)cmd; (void)buff;
   return RES_PARERR;
}
