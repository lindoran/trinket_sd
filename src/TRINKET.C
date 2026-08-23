/*-------------------------------------------------------------------------/
/  trinket.c -- diskio.h backend for the trinket SD-over-parallel-port
/  interface. Drop-in replacement for sdpp-main's SDMM.C: same public
/  contract (disk_initialize/disk_status/disk_result/disk_read/disk_write/
/  disk_ioctl/setportbase, plus the sd_card_check/portbase globals that
/  DRIVER.C's parse_options() already pokes), so SD.C, DRIVER.C and
/  HEADER.ASM are unmodified from sdpp-main.
/
/  Where SDMM.C bit-bangs SPI directly on four parallel-port data lines,
/  this module instead speaks to a trinket (RP2040 parallel-port-to-SPI
/  bridge) using its wire/dispatch/SPI protocol, then drives the SD card
/  in SPI mode over that link. Three layers, in the order they were
/  proven, each one built directly on top of the last:
/
/    Section 1 -- wire-level Init/Error byte + nibble handshakes.
/                 Ported line-for-line from trinket_ifacetest's
/                 driver/backend.c, which is the hardware-confirmed
/                 (TRINKET.SYS, real hardware, 2026-08-13) host-side half
/                 of this protocol. Only the asm idiom changes: backend.c
/                 is GCC extended asm (outb/inb) for ia16-elf-gcc; this
/                 uses Borland/Turbo C's outp()/inp() (<conio.h>), which
/                 is the same convention sdpp-main's own SDMM.C already
/                 used for parallel-port I/O in this toolchain -- no
/                 inline asm needed at all at this layer.
/
/    Section 2 -- DISPATCH_CMD_SPI framing (subsys_spi.h) on top of
/                 Section 1. Ported from trinket_ifacetest/client/sdtest.c
/                 Section 2, which mirrors subsys_spi.h's framing by hand.
/                 Device-side status is confirmed already carrying the
/                 doc sec 7 0x88 correction (subsys_spi.c fix, see
/                 sdtest.c's own header comment) -- this file does pure
/                 raw reconstruction, same as sdtest.c and backend.c's
/                 read paths.
/
/    Section 3 -- SD-in-SPI-mode command layer (CMD0/CMD8/ACMD41/CMD58/
/                 CMD16/CMD17/CMD24) on top of Section 2. Ported from
/                 trinket_ifacetest/client/sdtest.c Section 3, which is
/                 the one piece of this whole stack that has actually
/                 been run end-to-end against a real card (SDv2 init,
/                 FAT16 mount, dir listing, file reads all confirmed via
/                 SDTEST.EXE). No multi-block (CMD18/CMD25) support --
/                 sdtest.c never had it either, so disk_read/disk_write
/                 below just loop the single-block commands.
/
/  Confirmed on real hardware: Section 1 (as backend.c), and Section 3's
/  SD command sequencing (as sdtest.c, driven by an operator, not by a
/  resident driver). NOT yet confirmed on real hardware as a resident,
/  loop-driven .SYS module: Section 2's dispatch/SPI framing itself
/  (subsys_spi.c on the device side is compile-checked only -- see
/  trinket_ifacetest/README.md), and the specific way this file drives
/  all three sections back-to-back inside disk_initialize()/disk_read()/
/  disk_write() rather than a human running one SDTEST.EXE command at a
/  time. Bring this up in that order if something doesn't work: first
/  confirm ifacetest.c/DISPATCH_CMD_TEST_HARNESS against this same port
/  base to isolate "wire problem" from "SPI-subsystem problem", exactly
/  as sdtest.c's own header comment recommends.
/
/  NOT carried over from SDMM.C: card-detect (sd_card_check /k option,
/  CDDETECT()). Trinket's GPIO map (README.md) has no line dedicated to
/  SD card-detect over the parallel link in this design -- the sdpp
/  adapter's CD-on-BUSY wiring doesn't apply here. sd_card_check /
/  portbase stay as globals (DRIVER.C's parse_options() pokes both
/  directly and needs them to exist), but sd_card_check is otherwise
/  unused below -- /k on the CONFIG.SYS line is accepted and silently
/  ignored rather than doing anything.
/-------------------------------------------------------------------------*/

#include <conio.h>      /* inp/outp */

#include "diskio.h"     /* disk_* contract this module implements */
#include "cprint.h"     /* cdprintf, for init-time diagnostics */

/* ===================================================================
 * Section 0: globals DRIVER.C's parse_options() expects to exist
 * (mirrors SDMM.C exactly).
 * =================================================================== */

BYTE sd_card_check = 0;   /* /k -- accepted, not implemented, see above */
BYTE portbase = 2;        /* /b -- index into port_bases[], default 0x378 */

static WORD port_bases[5] = {0x3BCu, 0x378u, 0x278u, 0x3E8u, 0x2E8u};
static WORD base_port = 0x378u;

/* ===================================================================
 * Section 1: wire-level Init/Error byte + nibble handshakes.
 * Ported from trinket_ifacetest/driver/backend.c (hardware-confirmed).
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

#define POLL_TIMEOUT_ITERS  60000UL

static BYTE ctrl_shadow = CTRL_IDLE_DEFAULT;

static void ctrl_out(BYTE value)
{
   ctrl_shadow = value;
   outportbyte(base_port + REG_CONTROL, value);
}

/* Port 0x80 "diagnostic POST port" read -- timing-source-independent
 * ISA bus delay, same trick backend.c/sdtest.c use. No asm needed:
 * inp() already compiles to a plain IN instruction under Turbo C. */
static void io_delay(void)
{
   int i;
   for (i = 0; i < 4; i++)
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
   DWORD i;
   for (i = 0; i < POLL_TIMEOUT_ITERS; i++)
      if (inportbyte(base_port + REG_STATUS) & 0x08u) return 0;
   return 1;
}

static int wait_error_low(void)
{
   DWORD i;
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

/* backend_reset()'s equivalent -- assert nAuto-LF, hold, release. Called
 * once per disk_initialize() (see Section 4), same as do_init() calling
 * backend_reset() unconditionally on every TRINKET.SYS load. */
static void host_reset(void)
{
   ctrl_out((BYTE)(CTRL_AUTOLF_ASSERT | CTRL_INIT_DROPPED | CTRL_SELECT_WRITE));
   reset_hold_delay();
   ctrl_out(CTRL_IDLE_DEFAULT);
   io_delay();
}

/* One Init/Error byte-handshake cycle, host->device, direction already
 * Write. Returns 0 on success, 1 on timeout. */
static int raw_send_byte(BYTE b)
{
   outportbyte(base_port + REG_DATA, b);

   ctrl_out((BYTE)(ctrl_shadow | CTRL_INIT_RAISED));
   if (wait_error_high()) return 1;

   ctrl_out((BYTE)(ctrl_shadow & ~CTRL_INIT_RAISED));
   if (wait_error_low()) return 1;

   return 0;
}

/* One byte, two-cycle nibble handshake, device->host, direction already
 * Read. Raw reconstruction only -- doc sec 7's 0x88 correction is
 * applied on the trinket firmware side (subsys_spi.c), not here, same
 * as backend.c's read path. */
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

/* ===================================================================
 * Section 2: DISPATCH_CMD_SPI sub-protocol (subsys_spi.h framing).
 * Ported from trinket_ifacetest/client/sdtest.c Section 2.
 * =================================================================== */

#define DISPATCH_CMD_SPI  0x02u

#define SPI_OP_CONFIG   0x01u
#define SPI_OP_CS       0x02u
#define SPI_OP_XFER     0x03u

#define SPI_STATUS_OK     0x00u
#define SPI_STATUS_ERROR  0x01u

#define SPI_MAX_XFER  520u   /* must match trinket firmware's subsys_spi.c */

/* payload1/payload2 are DOSFAR so callers can pass either a small local
 * (near) array -- implicitly widened to far by Borland C -- or a DOS
 * request buffer that's already far (needed for the 512-byte data body
 * of a write, see sd_write_block() below). */
static int dispatch_send_spi_op(BYTE opcode,
   const BYTE DOSFAR *payload1, UINT n1,
   const BYTE DOSFAR *payload2, UINT n2)
{
   UINT i;

   host_select_direction(1);
   if (raw_send_byte(DISPATCH_CMD_SPI)) return 1;
   if (raw_send_byte(opcode)) return 1;
   for (i = 0; i < n1; i++)
      if (raw_send_byte(payload1[i])) return 1;
   for (i = 0; i < n2; i++)
      if (raw_send_byte(payload2[i])) return 1;
   return 0;
}

/* Fetches the result of the most recently completed SPI_OP_XFER. buf may
 * be NULL to discard (command-frame echoes nobody cares about). Bytes
 * beyond bufcap are still drained off the wire to stay in lockstep, same
 * as sdtest.c's version. */
static int spi_get_result(BYTE *out_status, BYTE DOSFAR *buf,
   UINT bufcap, UINT *out_len)
{
   BYTE status, len_lo, len_hi, b;
   UINT len, i;

   host_select_direction(1);
   if (raw_send_byte(DISPATCH_CMD_SPI)) return 1;
   host_select_direction(0);

   if (raw_recv_byte(&status)) return 1;
   if (raw_recv_byte(&len_lo)) return 1;
   if (raw_recv_byte(&len_hi)) return 1;
   len = (UINT)len_lo | ((UINT)len_hi << 8);

   for (i = 0; i < len; i++) {
      if (raw_recv_byte(&b)) return 1;
      if (buf != 0 && i < bufcap) buf[i] = b;
   }

   *out_status = status;
   if (out_len != 0) *out_len = len;
   return 0;
}

static int spi_op_config(BYTE mode, WORD khz)
{
   BYTE payload[3];
   payload[0] = mode;
   payload[1] = (BYTE)(khz & 0xFFu);
   payload[2] = (BYTE)((khz >> 8) & 0xFFu);
   return dispatch_send_spi_op(SPI_OP_CONFIG, payload, 3, 0, 0);
}

static int spi_op_cs(int assert_level)
{
   BYTE payload[1];
   payload[0] = (BYTE)(assert_level ? 1 : 0);
   return dispatch_send_spi_op(SPI_OP_CS, payload, 1, 0, 0);
}

/* tx/rx are DOSFAR for the same reason payload1/2 are above -- lets
 * sd_read_block()/sd_write_block() pass the caller's far sector buffer
 * straight through with no near-buffer copy. rx may be NULL to discard.
 * Returns 0 on success (*rx_len set), 1 on handshake failure, -1 if the
 * device reported SPI_STATUS_ERROR. */
static int spi_xfer(const BYTE DOSFAR *tx, UINT len,
   BYTE DOSFAR *rx, UINT rxcap, UINT *rx_len)
{
   BYTE hdr[2];
   BYTE status;
   UINT got_len;

   if (len > SPI_MAX_XFER) return 1;

   hdr[0] = (BYTE)(len & 0xFFu);
   hdr[1] = (BYTE)((len >> 8) & 0xFFu);

   if (dispatch_send_spi_op(SPI_OP_XFER, hdr, 2, tx, len)) return 1;

   if (spi_get_result(&status, rx, rxcap, &got_len)) return 1;
   if (status != SPI_STATUS_OK) return -1;
   if (rx_len != 0) *rx_len = got_len;
   return 0;
}

/* "Clock one byte, get one byte back." 0xFF on any failure -- also what
 * an idle card sends, so polling loops already treat the two the same
 * way (matches sdtest.c's spi_byte() convention). */
static BYTE spi_byte(BYTE tx_byte)
{
   BYTE tx[1], rx[1];
   UINT rx_len;

   tx[0] = tx_byte;
   if (spi_xfer(tx, 1, rx, 1, &rx_len) != 0 || rx_len != 1) return 0xFFu;
   return rx[0];
}

/* ===================================================================
 * Section 3: SD-in-SPI-mode command layer.
 * Ported from trinket_ifacetest/client/sdtest.c Section 3 -- the layer
 * that's actually been proven against real media (SDTEST.EXE).
 * =================================================================== */

static int g_block_addressed = 0;  /* 1 = SDHC/SDXC (CMD17/24 take a
                                     * block number); 0 = SDSC (byte
                                     * address = lba*512) */

static BYTE sd_cmd(BYTE cmd, DWORD arg)
{
   BYTE frame[6];
   BYTE r1;
   int i;

   frame[0] = (BYTE)(0x40u | cmd);
   frame[1] = (BYTE)(arg >> 24);
   frame[2] = (BYTE)(arg >> 16);
   frame[3] = (BYTE)(arg >> 8);
   frame[4] = (BYTE)(arg);
   if (cmd == 0)       frame[5] = 0x95u;
   else if (cmd == 8)  frame[5] = 0x87u;
   else                frame[5] = 0x01u;

   if (spi_xfer(frame, 6, 0, 0, 0) != 0) return 0xFFu;

   r1 = 0xFFu;
   for (i = 0; i < 8; i++) {
      r1 = spi_byte(0xFFu);
      if (!(r1 & 0x80u)) break;
   }
   return r1;
}

static BYTE sd_acmd41(DWORD arg)
{
   BYTE r1 = sd_cmd(55, 0);
   if (r1 & 0x80u) return r1;
   return sd_cmd(41, arg);
}

/* Standard SPI-mode bring-up: CMD0 -> CMD8 (SDv2 probe) -> ACMD41 loop
 * -> CMD58 (SDv2, CCS bit) or CMD16 (SDv1/MMC, force 512-byte blocks).
 * Returns 0 on success. Leaves clock at normal rate, CS deasserted. */
static int sd_init(void)
{
   BYTE r1;
   BYTE r7[4];
   int i;
   int is_v2 = 0;

   spi_op_config(0, 400u);   /* mode 0, 400kHz -- SD-safe init rate */
   spi_op_cs(0);
   {
      BYTE dummy[10];
      int j;
      for (j = 0; j < 10; j++) dummy[j] = 0xFFu;
      (void)spi_xfer(dummy, 10, 0, 0, 0);   /* >=74 dummy clocks, CS high */
   }

   spi_op_cs(1);
   for (i = 0; i < 10; i++) {
      r1 = sd_cmd(0, 0);
      if (r1 == 0x01u) break;
   }
   if (r1 != 0x01u) {
      cdprintf("TRINKET: CMD0 failed, r1=%x (no card / not responding)\n", r1);
      spi_op_cs(0);
      return -1;
   }

   r1 = sd_cmd(8, 0x1AAul);
   if (r1 == 0x01u) {
      BYTE rx[4], tx[4];
      int j;
      for (j = 0; j < 4; j++) tx[j] = 0xFFu;
      if (spi_xfer(tx, 4, rx, 4, 0) != 0) {
         cdprintf("TRINKET: CMD8 R7 read failed\n");
         spi_op_cs(0);
         return -1;
      }
      for (j = 0; j < 4; j++) r7[j] = rx[j];
      if (r7[2] != 0x01u || r7[3] != 0xAAu) {
         cdprintf("TRINKET: CMD8 pattern mismatch -- unsupported card\n");
         spi_op_cs(0);
         return -1;
      }
      is_v2 = 1;
   } else if (r1 & 0x04u) {
      is_v2 = 0;   /* illegal command -- SDv1 or MMC */
   } else {
      cdprintf("TRINKET: CMD8 unexpected r1=%x\n", r1);
      spi_op_cs(0);
      return -1;
   }

   for (i = 0; i < 1000; i++) {
      r1 = sd_acmd41(is_v2 ? 0x40000000ul : 0ul);
      if (r1 == 0x00u) break;
      if (r1 != 0x01u) {
         cdprintf("TRINKET: ACMD41 unexpected r1=%x\n", r1);
         spi_op_cs(0);
         return -1;
      }
   }
   if (r1 != 0x00u) {
      cdprintf("TRINKET: ACMD41 never left idle -- timeout\n");
      spi_op_cs(0);
      return -1;
   }

   if (is_v2) {
      BYTE rx[4], tx[4];
      int j;
      r1 = sd_cmd(58, 0);
      if (r1 != 0x00u) {
         cdprintf("TRINKET: CMD58 unexpected r1=%x\n", r1);
         spi_op_cs(0);
         return -1;
      }
      for (j = 0; j < 4; j++) tx[j] = 0xFFu;
      if (spi_xfer(tx, 4, rx, 4, 0) != 0) {
         cdprintf("TRINKET: CMD58 OCR read failed\n");
         spi_op_cs(0);
         return -1;
      }
      g_block_addressed = (rx[0] & 0x40u) ? 1 : 0;
   } else {
      r1 = sd_cmd(16, 512ul);
      if (r1 != 0x00u) {
         cdprintf("TRINKET: CMD16 (set blocklen 512) failed, r1=%x\n", r1);
         spi_op_cs(0);
         return -1;
      }
      g_block_addressed = 0;
   }

   spi_op_cs(0);
   spi_op_config(0, 4000u);   /* mode 0, up to 4MHz normal operation */

   cdprintf("TRINKET: card ready (%s, %s-addressed)\n",
      is_v2 ? "SDv2" : "SDv1/MMC",
      g_block_addressed ? "block" : "byte");
   return 0;
}

static DWORD sd_arg_for_lba(DWORD lba)
{
   return g_block_addressed ? lba : (lba * 512ul);
}

/* CMD17 READ_SINGLE_BLOCK straight into the caller's far buffer -- no
 * near-buffer copy, buf is exactly disk_read()'s far pointer for this
 * one sector. */
static int sd_read_block(DWORD lba, BYTE DOSFAR *buf)
{
   BYTE r1, token;
   int i;
   /* static, NOT a stack local: 512 bytes blew the small private stack
    * HEADER.ASM sets up for STRATEGY/INTERRUPT (it overlaps SD.C's own
    * 512-byte _LOCAL_BUFFER per TRINKET.MAP -- confirmed on hardware
    * 2026-08-22: JemmEx exception 0C / stack fault right after this
    * function's data phase, garbage SS:ESP, right by the STRATEGY/
    * INTERRUPT entry points). Content is constant 0xFF filler, so
    * static is not just safe here, it's the right storage class. */
   static BYTE tx[512];
   BYTE discard[2];

   spi_op_cs(1);
   r1 = sd_cmd(17, sd_arg_for_lba(lba));
   if (r1 != 0x00u) {
      cdprintf("TRINKET: CMD17 lba=%L r1=%x\n", lba, r1);
      spi_op_cs(0);
      return -1;
   }

   token = 0xFFu;
   for (i = 0; i < 8000; i++) {
      token = spi_byte(0xFFu);
      if (token != 0xFFu) break;
   }
   if (token != 0xFEu) {
      cdprintf("TRINKET: CMD17 lba=%L bad data token %x\n", lba, token);
      spi_op_cs(0);
      return -1;
   }

   /* tx here only needs to be 0xFF filler for the full-duplex clock;
    * it's near/local, widened to far automatically by spi_xfer()'s
    * DOSFAR parameter. */
   {
      int j;
      for (j = 0; j < 512; j++) tx[j] = 0xFFu;
   }
   if (spi_xfer(tx, 512, buf, 512, 0) != 0) {
      cdprintf("TRINKET: CMD17 lba=%L data read failed\n", lba);
      spi_op_cs(0);
      return -1;
   }
   (void)spi_xfer(tx, 2, discard, 2, 0);   /* trailing CRC, discarded */

   spi_op_cs(0);
   return 0;
}

/* CMD24 WRITE_BLOCK straight from the caller's far buffer. */
static int sd_write_block(DWORD lba, const BYTE DOSFAR *buf)
{
   BYTE r1, resp;
   BYTE token[1];
   BYTE crc[2];
   WORD i;   /* unsigned -- compared against 60000, which overflows a
              * signed 16-bit int and would make the busy-wait below
              * loop forever instead of timing out */

   spi_op_cs(1);
   r1 = sd_cmd(24, sd_arg_for_lba(lba));
   if (r1 != 0x00u) {
      cdprintf("TRINKET: CMD24 lba=%L r1=%x\n", lba, r1);
      spi_op_cs(0);
      return -1;
   }

   token[0] = 0xFEu;
   if (spi_xfer(token, 1, 0, 0, 0) != 0) {
      spi_op_cs(0);
      return -1;
   }

   if (spi_xfer(buf, 512, 0, 0, 0) != 0) {
      cdprintf("TRINKET: CMD24 lba=%L data write failed\n", lba);
      spi_op_cs(0);
      return -1;
   }
   crc[0] = 0xFFu; crc[1] = 0xFFu;
   (void)spi_xfer(crc, 2, 0, 0, 0);

   resp = spi_byte(0xFFu);
   if ((resp & 0x1Fu) != 0x05u) {
      cdprintf("TRINKET: CMD24 lba=%L data rejected, resp=%x\n", lba, resp);
      spi_op_cs(0);
      return -1;
   }

   /* Busy-wait: card holds MISO low while programming. Per sdtest.c's
    * own header comment, every byte here costs two dispatch round
    * trips -- this loop can legitimately take a couple hundred ms. */
   for (i = 0; i < 60000; i++)
      if (spi_byte(0xFFu) != 0x00u) break;

   spi_op_cs(0);
   return 0;
}

/* ===================================================================
 * Section 4: diskio.h public interface -- the contract SD.C/DRIVER.C
 * actually call (see this file's header comment for the confirmed
 * call list). Mirrors SDMM.C's shape so no caller changes.
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

DSTATUS disk_initialize(BYTE drv)
{
   setportbase(portbase);

   if (drv) return STA_NOINIT;

   host_reset();          /* known device state on every (re)init */

   if (sd_init() != 0) {
      Stat = STA_NOINIT;
      return Stat;
   }

   Stat = 0;
   return Stat;
}

DRESULT disk_read(BYTE drv, BYTE DOSFAR *buff, DWORD sector, UINT count)
{
   BYTE DOSFAR *p = buff;
   DRESULT dr = disk_result(drv);
   if (dr != RES_OK) return dr;

   while (count) {
      if (sd_read_block(sector, p) != 0) return RES_ERROR;
      sector++;  p += 512;  count--;
   }
   return RES_OK;
}

DRESULT disk_write(BYTE drv, const BYTE DOSFAR *buff, DWORD sector, UINT count)
{
   const BYTE DOSFAR *p = buff;
   DRESULT dr = disk_result(drv);
   if (dr != RES_OK) return dr;

   while (count) {
      if (sd_write_block(sector, p) != 0) return RES_ERROR;
      sector++;  p += 512;  count--;
   }
   return RES_OK;
}

/* Not exercised by SD.C/DRIVER.C at all (see this file's header
 * comment) -- kept only so the diskio.h contract compiles and links
 * cleanly. Every subcommand returns RES_PARERR ("not supported")
 * rather than silently pretending to implement something untested. */
DRESULT disk_ioctl(BYTE drv, BYTE cmd, void DOSFAR *buff)
{
   (void)drv; (void)cmd; (void)buff;
   return RES_PARERR;
}
