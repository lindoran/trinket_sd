/*
 * subsys_spi.h -- basic SPI master subsystem, selected via
 * DISPATCH_CMD_SPI (see dispatch.h). Not SD-specific: this is a general
 * "clock N bytes out, get N bytes back, control CS, pick a clock rate/
 * mode" bridge, same shape as e.g. an FT232H/CH347 USB-SPI adapter. Any
 * SD/FAT16-specific behavior (CMD0/CMD8/ACMD41 sequencing, R1/R7
 * response parsing, block CRCs, ...) is host-side logic for a future
 * TSR to build on top of this, not something this file knows about --
 * per the project's current direction, TRINKET.SYS itself stays a
 * dumb byte pipe (see driver/NOTES.md's "Character device, not block
 * device" section); this subsystem is the device-side half a future
 * block-device TSR will drive directly (most likely via its own raw
 * port I/O + its own dispatch-byte injection, the same pattern
 * driver/backend.c already uses for DISPATCH_CMD_TEST_HARNESS, not
 * through TRINKET.SYS's existing CMD_INPUT/CMD_OUTPUT byte semantics,
 * which know nothing about dispatch commands other than
 * DISPATCH_CMD_TEST_HARNESS -- see backend.c's header comment for that
 * hardcoding).
 *
 * Hardware: H3 header, GPIO0-3 (README.md's GPIO Map), driven with the
 * RP2040's real SPI0 peripheral (hardware_spi), not bit-banged:
 *   GPIO0  MISO  (SPI0 RX)
 *   GPIO1  SS    (manual GPIO, NOT the SPI peripheral's automatic CS --
 *                 SD cards and most SPI devices need CS held across
 *                 several transfers, which a hardware-CS peripheral
 *                 toggling it once per transfer can't do)
 *   GPIO2  SCK   (SPI0 SCK)
 *   GPIO3  MOSI  (SPI0 TX)
 *
 * ---------------------------------------------------------------------
 * Sub-protocol carried inside DISPATCH_CMD_SPI
 * ---------------------------------------------------------------------
 * dispatch.c has already consumed the DISPATCH_CMD_SPI command byte
 * (always a Write, per dispatch.h) by the time subsys_spi_run() is
 * called. From there, this subsystem owns everything about its own
 * next transfer, same freedom subsys_testharness_run() already has
 * (dispatch.h point 3) -- in particular, unlike the test harness, a
 * single subsys_spi_run() call is NOT limited to one wire-level
 * handshake: it loops wire_wait_init_high()/wire_finish_write_handshake()
 * (host->device bytes) or wire_wait_init_high()/wire_finish_read_cycle()
 * pairs (device->host bytes, 2 nibble cycles each, same as
 * subsys_testharness's read path) as many times as its op needs, all
 * still within the one dispatch cycle the host initiated. This keeps
 * every *top-level* transfer starting with its own dispatch byte (the
 * invariant dispatch.h documents) while letting a multi-byte SPI
 * request/response ride inside a single one of those transfers instead
 * of costing one dispatch round trip per byte.
 *
 * Which direction (Write/Read) the host selects before raising Init for
 * the FIRST byte of the request decides which of two things happens:
 *
 *   WRITE -- host is issuing a new SPI operation. First byte is an
 *   opcode (SPI_OP_* below); how many more bytes follow depends on the
 *   opcode, all still host->device write handshakes:
 *
 *     SPI_OP_CONFIG (0x01): mode(1), baud_khz_lo(1), baud_khz_hi(1).
 *       mode is 0-3 (CPOL/CPHA per the standard SPI mode numbering).
 *       baud_khz is the requested clock in kHz, little-endian, applied
 *       via spi_set_baudrate() -- the RP2040 picks the nearest rate its
 *       clock divider can hit and that's what's actually used; there is
 *       currently no way for the host to read back the ACTUAL rate
 *       chosen (see subsys_spi.c's TODO on this). Values above what the
 *       hardware can do are silently clamped by the SDK to its own
 *       maximum, same caveat.
 *
 *     SPI_OP_CS (0x02): level(1). 1 = assert (select the device --
 *       drives GPIO1 LOW, active-low CS being the overwhelmingly common
 *       convention this pin was wired for -- SD cards included). 0 =
 *       deassert (release -- GPIO1 HIGH). Left to the caller to hold
 *       across as many SPI_OP_XFER ops as it needs; this subsystem
 *       never touches CS on its own except at reset (see below).
 *
 *     SPI_OP_XFER (0x03): len_lo(1), len_hi(1), then len TX bytes.
 *       Performs one real, blocking, full-duplex SPI0 transaction of
 *       exactly `len` bytes (spi_write_read_blocking()) -- the RX bytes
 *       captured off MISO during that transaction are stashed
 *       internally, NOT sent back as part of this write; the host
 *       fetches them with a separate Read-direction dispatch cycle
 *       (below). len is capped at SPI_MAX_XFER (subsys_spi.c) -- a
 *       request over that cap is still fully drained from the wire
 *       (every TX byte the host sends gets its write handshake
 *       completed) so the two sides stay in lockstep, but no SPI bus
 *       transaction happens and the next Read reports SPI_STATUS_ERROR
 *       with zero bytes. This is a deliberate simplification for this
 *       first pass -- see subsys_spi.c's header comment.
 *
 *     Any other first byte: unrecognized opcode. Logged and the
 *     transfer ends there (same "log and drop" convention dispatch.c's
 *     own unknown-command case uses) -- no further bytes are read, so a
 *     host that sends a bad opcode followed by bytes it thought were
 *     payload will desync; this is a protocol-usage bug on the host
 *     side, not something this layer can recover from generically.
 *
 *   READ -- host wants the result of the most recently completed
 *   SPI_OP_XFER. Response, all sent device->host via nibble-read
 *   cycles (2 cycles/byte, same mechanic subsys_testharness's read path
 *   uses for its one byte):
 *
 *     status(1), len_lo(1), len_hi(1), then len RX bytes.
 *
 *     status is SPI_STATUS_OK (0) if the last SPI_OP_XFER completed
 *     normally, in which case len/data are that transfer's captured RX
 *     bytes. status is SPI_STATUS_ERROR (1) if there is no completed
 *     transfer to report (device just reset, or came up fresh) or the
 *     last SPI_OP_XFER was rejected for exceeding SPI_MAX_XFER, in
 *     which case len is 0 and no data bytes follow. Reading twice in a
 *     row without an intervening SPI_OP_XFER returns the same result
 *     both times (nothing is consumed/cleared by reading it, same
 *     "peek, not pop" semantics subsys_testharness's last_byte already
 *     has, and the same property driver.c's do_nd_input() relies on
 *     for that subsystem).
 *
 * Namespace note (see dispatch.h's own version of this note): SPI_OP_*
 * and SPI_STATUS_* below are a THIRD namespace, distinct from both
 * DISPATCH_CMD_* (dispatch.h) and device.h's DOS-side CMD_* / ERR_* --
 * three different protocols, at three different layers, that happen to
 * each be "small integers picked by hand." No numeric value is shared
 * or implied between them.
 * ---------------------------------------------------------------------
 */

#ifndef TRINKET_SUBSYS_SPI_H
#define TRINKET_SUBSYS_SPI_H

#include <stdbool.h>

/* SPI operation codes -- first byte of a Write-direction request. */
#define SPI_OP_CONFIG   0x01u   /* mode, baud_khz_lo, baud_khz_hi */
#define SPI_OP_CS       0x02u   /* level: 1=assert(select), 0=deassert */
#define SPI_OP_XFER     0x03u   /* len_lo, len_hi, then len TX bytes */

/* Status byte -- first byte of a Read-direction response. */
#define SPI_STATUS_OK     0x00u
#define SPI_STATUS_ERROR  0x01u

/* Call once at startup (main.c), after wire_setup_pins() -- brings up
 * SPI0 on GPIO0/2/3 and GPIO1 as a manual CS output, deasserted, at the
 * safe power-on-default rate/mode documented in subsys_spi.c. Separate
 * from subsys_spi_reset() the same way wire_setup_pins()/
 * wire_setup_reset_irq() are separate from wire_do_reset_sequence():
 * this is one-time hardware bring-up, reset is "return to that same
 * known state" on every nAuto-LF pulse. */
void subsys_spi_init(void);

/* Runs exactly one dispatch cycle's worth of SPI activity once
 * dispatched to -- see this file's header comment for the full
 * Write/Read sub-protocol. Returns false if wire_reset_pending() fired
 * at any point (same abort-propagation convention as
 * subsys_testharness_run() and everything in wire.h). */
bool subsys_spi_run(void);

/* Clears this subsystem's own state on an nAuto-LF reset: deasserts CS,
 * discards any unread SPI_OP_XFER result (a following Read reports
 * SPI_STATUS_ERROR until a new SPI_OP_XFER completes), and restores the
 * safe default clock rate/mode -- same "known idle state after reset"
 * guarantee wire_do_reset_sequence() gives the wire itself. main.c
 * calls this explicitly right after wire_do_reset_sequence() returns,
 * same as it already does for subsys_testharness_reset() -- wire.c
 * doesn't know this subsystem exists and can't clear state it doesn't
 * own. */
void subsys_spi_reset(void);

#endif /* TRINKET_SUBSYS_SPI_H */
