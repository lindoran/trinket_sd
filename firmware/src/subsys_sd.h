/*
 * subsys_sd.h -- high-level SD-over-SPI subsystem, selected via
 * DISPATCH_CMD_SD (see dispatch.h). This is ENHANCEMENTS.md point 1
 * ("High-Level Firmware Offloading"): where subsys_spi.c is a dumb
 * "clock N bytes, get N bytes back" bridge with all SD/FAT-specific
 * sequencing living host-side (TRINKET.C Section 3 / sdtest.c Section
 * 3), this subsystem moves that sequencing onto the RP2040 itself --
 * CMD0/CMD8/ACMD41/CMD58/CMD16/CMD17/CMD24, R1/R7 parsing, and data-
 * token polling all happen here, driving SPI0 directly
 * (spi_write_read_blocking()) with no per-SD-command trip back over
 * the parallel link. The host sends one high-level op (init / read N
 * sectors / write N sectors) and gets one result back, instead of the
 * dozens of DISPATCH_CMD_SPI round trips the same operation costs
 * today.
 *
 * Deliberately NOT included: CMD18/CMD25 real multi-block SD transfers
 * (ENHANCEMENTS.md point 2) -- SD_OP_READ_SECTORS/SD_OP_WRITE_SECTORS
 * below still loop single-block CMD17/CMD24 per sector internally, same
 * algorithm TRINKET.C uses today. The win here is entirely in
 * collapsing the PARALLEL-PORT round trips for a multi-sector request
 * from one dispatch cycle per sector down to one dispatch cycle for the
 * whole request; the SD-bus-level command count is unchanged. Point 2
 * is explicitly on hold pending the multi-subsystem/burst-transfer
 * design question, and slots in later as a change to sd_read_sectors()/
 * sd_write_sectors()'s internals below without touching this file's
 * wire protocol at all.
 *
 * Also included: SD_OP_CHECK_MEDIA (ENHANCEMENTS.md point 3's firmware-
 * side half). Point 3's host-side VolID caching is already done (SD.C's
 * SDMediaCheck, informally "0005") and still does a real disk_read(0,1)
 * every call to compare BS_VolID -- that's correct and stays as the
 * authority on "did the media actually change". SD_OP_CHECK_MEDIA is a
 * much cheaper *pre-filter* for the other common case a media check
 * needs to handle fast: the card being gone entirely. It answers "is
 * the card even responding" with one lightweight SD command (CMD13)
 * instead of a full 512-byte sector transfer with its data-token wait,
 * so a disconnected/dead card fails fast instead of running through
 * disk_read()'s full timeout/retry path. It deliberately does NOT and
 * cannot replace the VolID comparison -- a card that's electrically
 * present but has been swapped for a different one still answers CMD13
 * just fine, so "card responding" is necessary but not sufficient
 * evidence that it's the *same* card. Not yet wired into SD.C/
 * DRIVER.C -- that's the driver-migration step this subsystem's
 * SD_OP_INIT/READ/WRITE aren't wired into yet either.
 *
 * Hardware: same physical pins as subsys_spi.h (H3 header, GPIO0-3),
 * driven independently rather than through that subsystem -- see this
 * file's own subsys_sd_init() comment for why owning the hardware
 * redundantly (rather than depending on subsys_spi_init() having run
 * first) was the deliberate choice here.
 *
 * UNTESTED ON REAL HARDWARE: compiled clean against the real pico-sdk
 * 2.3.0 / arm-none-eabi-gcc 13.2 (first real build this project's
 * firmware has had -- previously inspection-only, see subsys_spi.c's
 * header comment for that history) but not yet run against an actual
 * SD card or even a logic analyzer. Bring this up in the same order
 * subsys_spi.c's header comment recommends: confirm DISPATCH_CMD_SD's
 * wire framing first (a `fastinit` from the updated sdtest.c client is
 * enough to prove the dispatch byte, opcode, and response framing all
 * round-trip correctly, even against a card that fails to init), only
 * then trust the actual SD command sequencing against real media.
 *
 * ---------------------------------------------------------------------
 * Sub-protocol carried inside DISPATCH_CMD_SD
 * ---------------------------------------------------------------------
 * Same two-phase shape as DISPATCH_CMD_SPI (subsys_spi.h): a WRITE-
 * direction dispatch cycle submits one operation (and, for
 * SD_OP_WRITE_SECTORS, its payload); a separate, later READ-direction
 * dispatch cycle fetches that operation's result. Unlike
 * DISPATCH_CMD_SPI, the WRITE side here can itself take a while to
 * return (SD_OP_INIT runs the whole CMD0..ACMD41 bring-up; a multi-
 * sector SD_OP_READ_SECTORS/SD_OP_WRITE_SECTORS runs `count` real SD
 * transactions) -- this is fine, dispatch_run_one()/main.c's loop is
 * happy to block for as long as a subsystem's run() call needs, same
 * as subsys_spi.c's handle_op_xfer() already does for one
 * spi_write_read_blocking() call.
 *
 *   WRITE -- host is issuing a new SD operation. First byte is an
 *   opcode (SD_OP_* below); how many more bytes follow depends on the
 *   opcode, all still host->device write handshakes:
 *
 *     SD_OP_INIT (0x01): no further bytes. Runs the standard SPI-mode
 *       bring-up (CMD0 -> CMD8 -> ACMD41 loop -> CMD58 or CMD16),
 *       exactly TRINKET.C's sd_init() algorithm, against SPI0 directly.
 *       Leaves clock at normal rate (SD_NORMAL_BAUD_HZ), CS deasserted.
 *
 *     SD_OP_READ_SECTORS (0x02): lba_b0(1,LSB) lba_b1(1) lba_b2(1)
 *       lba_b3(1,MSB), count(1). Reads `count` consecutive 512-byte
 *       sectors starting at `lba` into an internal buffer via `count`
 *       sequential CMD17s. count above SD_MAX_SECTORS is rejected (see
 *       SD_STATUS_ERROR below) -- the host driver must already chunk
 *       its own requests to this cap or smaller (DRIVER.C's ReadBlock/
 *       WriteBlock already cap at 16 sectors/call for an unrelated
 *       reason, well under SD_MAX_SECTORS, so this is not expected to
 *       bind in practice).
 *
 *     SD_OP_WRITE_SECTORS (0x03): lba_b0..b3(4), count(1), then
 *       count*512 raw payload bytes, all still host->device write
 *       handshakes (this is the one op where the "payload" is large --
 *       up to SD_MAX_SECTORS*512 bytes, all drained off the wire
 *       before any SD-bus activity happens, mirroring subsys_spi.c's
 *       handle_op_xfer() TX phase). Writes `count` consecutive 512-byte
 *       sectors via `count` sequential CMD24s.
 *
 *     SD_OP_CHECK_MEDIA (0x04): no further bytes. Issues one CMD13
 *       (SEND_STATUS) and reports whether a valid R1 came back within
 *       the same short retry budget sd_cmd() always uses -- SD_STATUS_OK
 *       means the card answered, SD_STATUS_ERROR means it didn't (or no
 *       SD_OP_INIT has succeeded yet this session, checked first without
 *       touching the bus). See this file's header comment above for what
 *       this does and doesn't tell you.
 *
 *     Any other first byte: unrecognized opcode, logged and dropped --
 *     same "log and drop, host will desync" convention as
 *     subsys_spi.h's own unknown-opcode case.
 *
 *   READ -- host wants the result of the most recently completed
 *   SD_OP_*. Response, all sent device->host via nibble-read cycles:
 *
 *     status(1), is_v2(1), block_addressed(1), len_lo(1), len_hi(1),
 *     then len data bytes.
 *
 *     status is SD_STATUS_OK (0) if the last operation completed
 *     normally, SD_STATUS_ERROR (1) otherwise (card didn't respond,
 *     bad token, oversize count, ...) -- see the per-opcode handler
 *     comments in subsys_sd.c for exactly what's checked.
 *
 *     is_v2/block_addressed reflect the card state as of the last
 *     successful SD_OP_INIT (0/0 if no card has ever been successfully
 *     inited this session) -- included on every response, not just
 *     after SD_OP_INIT, so the host can log/sanity-check them whenever
 *     convenient without an extra round trip; they're meaningless
 *     noise to ignore on an SD_OP_READ_SECTORS/SD_OP_WRITE_SECTORS
 *     response.
 *
 *     len/data are the `count*512` bytes read by the last successful
 *     SD_OP_READ_SECTORS (0 for SD_OP_INIT and SD_OP_WRITE_SECTORS, or
 *     any failed operation). Same "peek, not pop" semantics as
 *     subsys_spi.h's response -- reading twice without an intervening
 *     WRITE returns the same thing both times.
 *
 * Namespace note (see dispatch.h/subsys_spi.h's own versions of this
 * note): SD_OP_* and SD_STATUS_* below are their own namespace, not
 * shared with SPI_OP_* / SPI_STATUS_* or anything else in this project.
 * ---------------------------------------------------------------------
 */

#ifndef TRINKET_SUBSYS_SD_H
#define TRINKET_SUBSYS_SD_H

#include <stdbool.h>

/* SD operation codes -- first byte of a Write-direction request. */
#define SD_OP_INIT           0x01u   /* no payload */
#define SD_OP_READ_SECTORS   0x02u   /* lba(4,LE), count(1) */
#define SD_OP_WRITE_SECTORS  0x03u   /* lba(4,LE), count(1), count*512 data */
#define SD_OP_CHECK_MEDIA    0x04u   /* no payload */

/* Status byte -- first byte of a Read-direction response. */
#define SD_STATUS_OK     0x00u
#define SD_STATUS_ERROR  0x01u

/* Cap on sectors per single SD_OP_READ_SECTORS/SD_OP_WRITE_SECTORS --
 * bounds the internal buffer (SD_MAX_SECTORS*512 bytes, static, see
 * subsys_sd.c). 64 sectors (32KB) is generous headroom over DRIVER.C's
 * existing 16-sector/call cap and cheap on the RP2040's 264KB SRAM;
 * revisit if a future CMD18/25 burst design (point 2) wants bigger. */
#define SD_MAX_SECTORS  64u

/* Call once at startup (main.c), after wire_setup_pins(). Brings up
 * SPI0 (GPIO0/2/3) and GPIO1 as manual CS, deasserted, at the same
 * safe power-on default subsys_spi_init() uses. Deliberately
 * redundant with subsys_spi_init() rather than depending on call
 * order between the two: each subsystem in this project owns its own
 * hardware bring-up idempotently (see subsys_spi.c precedent) rather
 * than one silently depending on another having run first --
 * spi_init()/gpio_init() on the same peripheral/pin from two places at
 * startup is harmless (register writes, not stateful negotiation), and
 * keeping this file self-contained means it can be lifted into a
 * project that never had subsys_spi.c at all. Whichever of this or
 * subsys_spi.c the host uses last for an actual transfer is what
 * matters for the shared hardware's live config, since the two are
 * never in concurrent use (one dispatch cycle at a time, host chooses
 * the command byte). */
void subsys_sd_init(void);

/* Runs exactly one dispatch cycle's worth of SD activity -- see this
 * file's header comment for the full Write/Read sub-protocol. Returns
 * false if wire_reset_pending() fired at any point (same abort-
 * propagation convention as subsys_spi_run() and everything in
 * wire.h). */
bool subsys_sd_run(void);

/* Resets the subsystem's protocol/framing state on an nAuto-LF reset:
 * deasserts CS, discards any unread SD_OP_READ_SECTORS result (a
 * following Read reports SD_STATUS_ERROR until a new op completes),
 * and restores the safe default clock rate/mode. This reset does not
 * reinitialize the SD card itself, so it intentionally does not clear
 * sd_initialized: the card remains initialized across a wire-protocol
 * reset, and a fresh SD_OP_INIT is still the only legitimate way to
 * establish a new card state after power-up or a failed re-init. Does
 * NOT reset is_v2/block_addressed to 0 -- those reflect "what the last
 * successful init found" and remain meaningful to report across a
 * framing-only reset. A caller relying on current card state after a
 * reset should re-issue SD_OP_INIT regardless, same as TRINKET.C's
 * disk_initialize() already does unconditionally on every (re)load.
 * main.c calls this right after wire_do_reset_sequence(), same as
 * subsys_spi_reset(). */
void subsys_sd_reset(void);

/* Enables/disables this subsystem's routine per-operation printf()
 * lines, same off-by-default/ESC-toggle convention as
 * subsys_spi_set_debug() -- see that function's header comment. Does
 * NOT gate error-path printfs (CMD0/8/ACMD41 failures, bad data
 * tokens, oversize count) -- those stay visible regardless. */
void subsys_sd_set_debug(bool enabled);

#endif /* TRINKET_SUBSYS_SD_H */
