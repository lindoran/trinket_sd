/*
 * subsys_spi.c -- see subsys_spi.h for the sub-protocol this implements.
 *
 * UNTESTED ON REAL HARDWARE / UNBUILT. Same status this whole project
 * has consistently called out for new code before it's actually been
 * run through the real toolchain (see README.md, driver/NOTES.md's
 * "Toolchain assumptions to confirm" and build log) -- there is no
 * arm-none-eabi-gcc / vendored pico-sdk available in the environment
 * this was written in, so this has only been checked by inspection
 * against the SDK's documented hardware_spi/hardware_gpio APIs
 * (spi_init/spi_set_format/spi_set_baudrate/spi_write_read_blocking,
 * gpio_set_function/gpio_init/gpio_set_dir/gpio_put), not compiled.
 * Build against the real SDK and run it against a real SPI device
 * (logic analyzer on GPIO0-3 first, then an actual SD card once the
 * FAT16 TSR exists) before trusting the framing below, the same "should
 * be right by inspection, not confirmed" caveat dispatch.c/wire.c's
 * split carried until reset and the dispatcher itself were separately
 * proven on hardware.
 *
 * Deliberate first-pass simplifications, to flag for whoever picks this
 * up next:
 *   - SPI_MAX_XFER (below) caps a single SPI_OP_XFER at 520 bytes --
 *     enough for a 512-byte SD data block plus its 2-byte CRC and a
 *     little slack for command/response bytes in the same transaction,
 *     but arbitrary beyond "should cover the FAT16 TSR's obvious case."
 *     Revisit once that TSR exists and its real access patterns are
 *     known.
 *   - No way for the host to read back the ACTUAL clock rate
 *     spi_set_baudrate() chose (the RP2040's integer clock divider
 *     won't hit arbitrary rates exactly). SPI_OP_CONFIG is fire-and-
 *     forget from the host's perspective right now. A future
 *     SPI_OP_GET_CONFIG (or folding the achieved rate into the
 *     SPI_OP_CONFIG write's own... there's no response channel for a
 *     Write op as this is designed, so that would need to be a new
 *     Read-direction case) would close this gap if a caller ever needs
 *     to know precisely, rather than approximately, what rate it's
 *     running at.
 *   - One SPI0 transaction per SPI_OP_XFER, fully blocking
 *     (spi_write_read_blocking spins the CPU for the transaction's
 *     duration) -- fine at "basic SPI" scope with nothing else for this
 *     firmware to be doing meanwhile, worth reconsidering if a future
 *     subsystem needs to run concurrently with SPI traffic.
 */

#include "subsys_spi.h"

#include <stdint.h>
#include <string.h>

#include "debug.h"
#include "hardware/spi.h"
#include "hardware/gpio.h"

#include "wire.h"

// ---------------------------------------------------------------------
// Pin assignments -- H3 header, README.md's GPIO Map. GPIO1 (SS) is
// deliberately NOT given to the SPI peripheral's automatic hardware CS:
// SD cards (and most SPI devices worth talking to) need CS held low
// across several transactions -- e.g. send a command, then poll for a
// response byte in a follow-up transfer, all under one CS assertion --
// which a peripheral that toggles its own CS once per transaction can't
// do. Manual GPIO gives SPI_OP_CS full control of exactly when it moves.
// ---------------------------------------------------------------------
#define PIN_SPI_MISO   0
#define PIN_SPI_CS     1
#define PIN_SPI_SCK    2
#define PIN_SPI_MOSI   3

#define SPI_PORT  spi0

// Safe power-on-default: SD cards require <= 400kHz during their own
// power-up/CMD0-CMD8-ACMD41 sequence, mode 0 is the near-universal SPI
// default. Both subsys_spi_init() and subsys_spi_reset() apply this --
// see subsys_spi.h's header comment on why reset re-applies it rather
// than leaving whatever rate/mode was last configured.
#define SPI_DEFAULT_BAUD_HZ   400000u
#define SPI_DEFAULT_MODE      0u

// See this file's header comment -- arbitrary but generously sized
// for a first pass (512-byte SD block + 2-byte CRC + slack).
#define SPI_MAX_XFER   520u

static uint8_t rx_buf[SPI_MAX_XFER];
static uint16_t rx_len;          // valid bytes in rx_buf from the last completed SPI_OP_XFER
static uint8_t last_status;      // SPI_STATUS_OK / SPI_STATUS_ERROR, reported on the next Read

// This local flag gates routine/no-error messages for this subsystem.
// debug_printf() provides the global off-by-default USB-CDC lockdown for
// both these messages and the error-path messages below.
static bool debug_enabled = false;

void subsys_spi_set_debug(bool enabled) {
    debug_enabled = enabled;
}

// A single shared scratch buffer for the TX bytes an SPI_OP_XFER reads
// off the wire before handing them to spi_write_read_blocking(). Static
// (not a run()-local array) purely to keep this off the tiny driver-
// stack-sized call chain wire.c's handshakes already run under --
// see driver/NOTES.md's "STACK OVERFLOW" build-log entry for why that
// margin matters on the DOS side; the Pico side has a much larger
// stack, but there's no reason to burn 520 bytes of it here regardless.
static uint8_t tx_buf[SPI_MAX_XFER];

static void apply_mode(uint mode) {
    spi_cpol_t cpol = (mode & 0x02u) ? SPI_CPOL_1 : SPI_CPOL_0;
    spi_cpha_t cpha = (mode & 0x01u) ? SPI_CPHA_1 : SPI_CPHA_0;
    spi_set_format(SPI_PORT, 8, cpol, cpha, SPI_MSB_FIRST);
}

static void cs_assert(void) {
    gpio_put(PIN_SPI_CS, 0);   // active-low
}

static void cs_deassert(void) {
    gpio_put(PIN_SPI_CS, 1);
}

void subsys_spi_init(void) {
    spi_init(SPI_PORT, SPI_DEFAULT_BAUD_HZ);
    apply_mode(SPI_DEFAULT_MODE);

    gpio_set_function(PIN_SPI_MISO, GPIO_FUNC_SPI);
    gpio_set_function(PIN_SPI_SCK,  GPIO_FUNC_SPI);
    gpio_set_function(PIN_SPI_MOSI, GPIO_FUNC_SPI);

    gpio_init(PIN_SPI_CS);
    gpio_set_dir(PIN_SPI_CS, GPIO_OUT);
    cs_deassert();

    rx_len = 0;
    last_status = SPI_STATUS_ERROR;   // nothing transferred yet
}

void subsys_spi_reset(void) {
    cs_deassert();
    spi_set_baudrate(SPI_PORT, SPI_DEFAULT_BAUD_HZ);
    apply_mode(SPI_DEFAULT_MODE);

    // debug_enabled is deliberately NOT touched here: it's an
    // operator's console setting for the current bring-up/timing
    // session (main.c's ESC toggle), not link/SD-transaction state --
    // resetting it on every nAuto-LF pulse (which TRINKET.C's
    // disk_initialize() triggers on every driver load) would silently
    // undo the operator's choice each time DOS reinitializes.

    rx_len = 0;
    last_status = SPI_STATUS_ERROR;
}

// ---------------------------------------------------------------------
// Wire-level helpers -- multi-byte host->device / device->host bytes
// within a single dispatch cycle. Each mirrors subsys_testharness_run()
// exactly for one byte's worth of protocol (see wire.h's own comments
// for why the shapes below are what they are); these just let a whole
// run() call loop that per-byte shape without duplicating it at every
// call site.
// ---------------------------------------------------------------------

// Reads one more host->device byte. Caller must already know Init is
// about to go high for THIS byte (true for the very first byte because
// dispatch.c / this function's own caller already waited for it to
// select Write; true for every byte after that because this function
// itself waits before each one). Returns false on reset_pending, same
// convention as everything in wire.h.
static bool recv_next_byte(uint8_t *out) {
    if (!wire_wait_init_high()) {
        return false;
    }
    return wire_finish_write_handshake(out);
}

// Sends one more device->host byte via the two-cycle nibble handshake
// (doc sec 5.1), given Init is already high for this byte's first
// cycle -- true for the first byte because the caller already sampled
// direction via wire_wait_init_high() at the top of subsys_spi_run(),
// true for every byte after via the wire_wait_init_high() below.
//
// CONFIRMED ON REAL HARDWARE (see docs/CODEGEN_NOTES.md): this
// function originally sent `byte`'s nibbles unmodified, on the theory
// that subsys_testharness_run()'s `corrected = last_byte ^ 0x88; //
// doc sec 7` was specific to that subsystem's own echo-validation
// semantics, not something every Read-direction transmitter needs. A
// real-hardware run proved that wrong: every byte this subsystem sent
// came back at the host with 0x88 XORed in (status 0x00 arrived as
// 0x88, a length of 6 arrived as 0x8E, etc. -- see the CODEGEN_NOTES
// entry for the exact trace). "doc sec 7" is a wire-protocol-level
// requirement for the nibble-read mechanism itself, not test-harness-
// specific -- every subsystem that sends bytes device->host via
// wire_finish_read_cycle() needs this same correction, which is why it
// now lives here, in the shared low-level sender, rather than being
// something each caller has to remember (the same "make the mistake
// structurally impossible" fix as dispatch_send_spi_op() got on the
// client side for a related bug).
static bool send_next_byte(uint8_t byte) {
    uint8_t corrected = byte ^ 0x88u;                          // doc sec 7
    uint8_t low_nibble  = corrected & 0x0Fu;
    uint8_t high_nibble = (corrected >> 4) & 0x0Fu;

    if (!wire_finish_read_cycle(low_nibble, high_nibble)) {   // cycle 1
        return false;
    }
    if (!wire_wait_init_high()) {                              // cycle 2
        return false;
    }
    if (!wire_finish_read_cycle(low_nibble, high_nibble)) {
        return false;
    }
    return true;
}

// Like send_next_byte(), but for every byte AFTER the first one in a
// multi-byte response -- needs its own leading wait, since the
// previous byte's send_next_byte() left Init low (post-cycle-2) rather
// than high.
static bool send_subsequent_byte(uint8_t byte) {
    if (!wire_wait_init_high()) {
        return false;
    }
    return send_next_byte(byte);
}

// ---------------------------------------------------------------------
// Write-direction: host is issuing a new operation.
// ---------------------------------------------------------------------

static bool handle_op_config(void) {
    uint8_t mode, khz_lo, khz_hi;

    if (!recv_next_byte(&mode))   return false;
    if (!recv_next_byte(&khz_lo)) return false;
    if (!recv_next_byte(&khz_hi)) return false;

    uint32_t hz = ((uint32_t)khz_lo | ((uint32_t)khz_hi << 8)) * 1000u;
    if (hz == 0) {
        // A zero rate isn't meaningful hardware-wise; fall back to the
        // safe default rather than handing spi_set_baudrate(0) to the
        // SDK and finding out what it does with that.
        hz = SPI_DEFAULT_BAUD_HZ;
    }

    spi_set_baudrate(SPI_PORT, hz);
    apply_mode(mode & 0x03u);

    if (debug_enabled) {
        debug_printf("SPI: config mode=%u requested=%luHz\n", (unsigned)(mode & 0x03u), (unsigned long)hz);
    }
    return true;
}

static bool handle_op_cs(void) {
    uint8_t level;

    if (!recv_next_byte(&level)) return false;

    if (level) {
        cs_assert();
        if (debug_enabled) debug_printf("SPI: CS asserted\n");
    } else {
        cs_deassert();
        if (debug_enabled) debug_printf("SPI: CS deasserted\n");
    }
    return true;
}

static bool handle_op_xfer(void) {
    uint8_t len_lo, len_hi;

    if (!recv_next_byte(&len_lo)) return false;
    if (!recv_next_byte(&len_hi)) return false;

    uint16_t len = (uint16_t)len_lo | ((uint16_t)len_hi << 8);
    bool oversize = (len > SPI_MAX_XFER);

    // Either way, every TX byte the host committed to sending has to be
    // drained via a full write handshake, or the two sides desync on
    // the very next dispatch cycle -- see subsys_spi.h's header comment
    // on why an oversize request is "reported as an error on the next
    // Read", not "aborted here". Bytes are read into tx_buf up to the
    // cap and simply dropped past it; tx_buf is sized SPI_MAX_XFER, and
    // len is > SPI_MAX_XFER exactly when oversize is true, so the loop
    // bound below never indexes past it.
    uint16_t store_len = oversize ? SPI_MAX_XFER : len;
    uint16_t i;
    for (i = 0; i < len; i++) {
        uint8_t b;
        if (!recv_next_byte(&b)) {
            return false;
        }
        if (i < store_len) {
            tx_buf[i] = b;
        }
    }

    if (oversize) {
        debug_printf("SPI: XFER len=%u exceeds SPI_MAX_XFER=%u -- rejected\n",
               (unsigned)len, (unsigned)SPI_MAX_XFER);
        rx_len = 0;
        last_status = SPI_STATUS_ERROR;
        return true;
    }

    int rc = spi_write_read_blocking(SPI_PORT, tx_buf, rx_buf, len);
    if (rc < 0 || (uint16_t)rc != len) {
        debug_printf("SPI: XFER len=%u spi_write_read_blocking rc=%d -- error\n", (unsigned)len, rc);
        rx_len = 0;
        last_status = SPI_STATUS_ERROR;
        return true;
    }

    rx_len = len;
    last_status = SPI_STATUS_OK;
    if (debug_enabled) debug_printf("SPI: XFER len=%u ok\n", (unsigned)len);
    return true;
}

static bool run_write_side(void) {
    uint8_t opcode;

    if (!wire_finish_write_handshake(&opcode)) {
        return false;
    }

    switch (opcode) {
        case SPI_OP_CONFIG:
            return handle_op_config();
        case SPI_OP_CS:
            return handle_op_cs();
        case SPI_OP_XFER:
            return handle_op_xfer();
        default:
            // Same "log and drop the rest of this transfer" convention
            // as dispatch.c's own unknown-command case -- see
            // subsys_spi.h's header comment on why any payload the
            // host meant to follow a bad opcode can't be recovered
            // generically from here.
            debug_printf("SPI: unknown op 0x%02X -- ignoring\n", opcode);
            return true;
    }
}

// ---------------------------------------------------------------------
// Read-direction: host wants the result of the last completed
// SPI_OP_XFER. Response: status, len_lo, len_hi, then len data bytes --
// see subsys_spi.h's header comment for the exact framing and the
// "peek, not pop" semantics (reading again without a new SPI_OP_XFER in
// between returns the same thing).
// ---------------------------------------------------------------------

static bool run_read_side(void) {
    if (!send_next_byte(last_status)) {
        return false;
    }
    if (!send_subsequent_byte((uint8_t)(rx_len & 0xFFu))) {
        return false;
    }
    if (!send_subsequent_byte((uint8_t)((rx_len >> 8) & 0xFFu))) {
        return false;
    }

    uint16_t i;
    for (i = 0; i < rx_len; i++) {
        if (!send_subsequent_byte(rx_buf[i])) {
            return false;
        }
    }

    if (debug_enabled) {
        debug_printf("SPI: read result status=%u len=%u\n", (unsigned)last_status, (unsigned)rx_len);
    }
    return true;
}

bool subsys_spi_run(void) {
    // Same race-avoidance rule subsys_testharness_run() documents:
    // sample nSelect-In only once Init has actually gone high for this
    // byte, never speculatively beforehand.
    if (!wire_wait_init_high()) {
        return false;
    }

    if (wire_is_write_selected()) {
        return run_write_side();
    }
    return run_read_side();
}
