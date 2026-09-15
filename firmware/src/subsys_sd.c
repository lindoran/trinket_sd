/*
 * subsys_sd.c -- see subsys_sd.h for the sub-protocol this implements.
 *
 * The SD command sequencing below (sd_cmd/sd_acmd41/sd_do_init/
 * sd_read_one_sector/sd_write_one_sector/sd_arg_for_lba) is a direct
 * port of TRINKET.C's Section 3 (the one piece of this whole project
 * that's actually been run end-to-end against a real card, as
 * SDTEST.EXE) -- same algorithm, same command sequence, same magic
 * numbers (CRC bytes 0x95/0x87, data token 0xFE, busy-wait shape).
 * The only real difference is mechanical: TRINKET.C's version sends
 * each SPI byte over the parallel link via spi_xfer() (a full
 * DISPATCH_CMD_SPI round trip per SPI transaction); this version calls
 * spi_write_read_blocking() directly since it already IS the SPI
 * master, on the same die as the card. There's no 8086/Borland
 * cs.lib angle here at all -- this is arm-none-eabi-gcc on a real
 * 32-bit Cortex-M0+, so plain uint32_t arithmetic (sd_arg_for_lba's
 * multiply included) is completely ordinary, not the hazard it was on
 * the DOS side.
 *
 * Compiled clean against real pico-sdk 2.3.0 + arm-none-eabi-gcc
 * 13.2.1 (see this file's header-comment-mirror in subsys_sd.h for
 * what "compiled" does and doesn't establish here). NOT yet run
 * against real SPI0 hardware or an actual SD card.
 */

#include "subsys_sd.h"

#include <stdio.h>
#include <stdint.h>

#include "debug.h"
#include "hardware/spi.h"
#include "hardware/gpio.h"

#include "wire.h"
#include "subsys_dac.h"

// ---------------------------------------------------------------------
// Pin assignments -- same physical pins as subsys_spi.c (H3 header,
// README.md's GPIO Map); see subsys_sd.h's header comment on why this
// subsystem brings the hardware up itself rather than sharing
// subsys_spi.c's init.
// ---------------------------------------------------------------------
#define PIN_SPI_MISO   0
#define PIN_SPI_CS     1
#define PIN_SPI_SCK    2
#define PIN_SPI_MOSI   3

#define SPI_PORT  spi0

#define SD_INIT_BAUD_HZ    400000u   // <=400kHz required during CMD0..ACMD41
#define SD_NORMAL_BAUD_HZ  4000000u  // matches TRINKET.C's post-init rate
#define SD_SPI_MODE        0u

static uint8_t sd_buf[SD_MAX_SECTORS * 512u];   // shared read/write payload buffer
static uint8_t sd_write_discard[512];           // RX scratch for the data phase of
                                                 // sd_write_one_sector() -- NOT sd_buf,
                                                 // since `in` there points INTO sd_buf
                                                 // (see handle_op_write_sectors()) and
                                                 // using sd_buf itself as the RX side
                                                 // would clobber a not-yet-written
                                                 // sector while writing an earlier one
static uint16_t sd_last_len;                     // valid bytes in sd_buf for the next Read
static uint8_t sd_last_status;                   // SD_STATUS_OK / SD_STATUS_ERROR
static bool sd_is_v2;                            // from the last successful SD_OP_INIT
static bool sd_block_addressed;                  // ditto
static bool sd_initialized;                      // has SD_OP_INIT ever succeeded this session; left alone across a protocol reset because the card itself is not reinitialized by that reset

static bool debug_enabled = false;

void subsys_sd_set_debug(bool enabled) {
    debug_enabled = enabled;
}

static void apply_mode(void) {
    spi_set_format(SPI_PORT, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
}

static uint8_t spi_byte(uint8_t tx_byte) {
    uint8_t rx;
    spi_write_read_blocking(SPI_PORT, &tx_byte, &rx, 1);
    return rx;
}

static void cs_assert(void)   { gpio_put(PIN_SPI_CS, 0); }   // active-low
static void cs_deassert(void) {
    gpio_put(PIN_SPI_CS, 1);
    spi_byte(0xFFu);   // 8 dummy clocks with CS high to release DO/MISO and reset card command state machine
}

void subsys_sd_init(void) {
    spi_init(SPI_PORT, SD_INIT_BAUD_HZ);
    apply_mode();

    gpio_set_function(PIN_SPI_MISO, GPIO_FUNC_SPI);
    gpio_set_function(PIN_SPI_SCK,  GPIO_FUNC_SPI);
    gpio_set_function(PIN_SPI_MOSI, GPIO_FUNC_SPI);

    gpio_init(PIN_SPI_CS);
    gpio_set_dir(PIN_SPI_CS, GPIO_OUT);
    cs_deassert();

    sd_last_len = 0;
    sd_last_status = SD_STATUS_ERROR;
    sd_is_v2 = false;
    sd_block_addressed = false;
    sd_initialized = false;
}

void subsys_sd_reset(void) {
    cs_deassert();
    if (sd_initialized) {
        spi_set_baudrate(SPI_PORT, SD_NORMAL_BAUD_HZ);
    } else {
        spi_set_baudrate(SPI_PORT, SD_INIT_BAUD_HZ);
    }
    apply_mode();

    sd_last_len = 0;
    sd_last_status = SD_STATUS_ERROR;
    // Do not clear sd_initialized here: an nAuto-LF framing reset only
    // resynchronizes the parallel-port protocol state and does not power-
    // cycle or reinitialize the SD card itself. The card's SPI init state
    // persists across this reset, so a fresh SD_OP_INIT is the only thing
    // that legitimately clears the "card is live" flag.
    // sd_is_v2/sd_block_addressed deliberately left alone -- see
    // subsys_sd_reset()'s header comment in subsys_sd.h.
}

// ---------------------------------------------------------------------
// SD-in-SPI-mode command layer -- direct port of TRINKET.C Section 3.
// Every byte here is one real spi_write_read_blocking() call against
// SPI0; no parallel-link round trip anywhere in this section.
// ---------------------------------------------------------------------

static uint8_t sd_cmd(uint8_t cmd, uint32_t arg) {
    uint8_t frame[6];
    uint8_t discard[6];
    uint8_t r1;
    int i;

    frame[0] = 0x40u | cmd;
    frame[1] = (uint8_t)(arg >> 24);
    frame[2] = (uint8_t)(arg >> 16);
    frame[3] = (uint8_t)(arg >> 8);
    frame[4] = (uint8_t)(arg);
    if (cmd == 0)       frame[5] = 0x95u;
    else if (cmd == 8)  frame[5] = 0x87u;
    else                frame[5] = 0x01u;

    spi_byte(0xFFu);   // 8 dummy clocks before command frame to ensure SPI sync

    spi_write_read_blocking(SPI_PORT, frame, discard, 6);

    r1 = 0xFFu;
    for (i = 0; i < 10; i++) {
        r1 = spi_byte(0xFFu);
        if (!(r1 & 0x80u)) break;
    }
    return r1;
}

static uint8_t sd_acmd41(uint32_t arg) {
    uint8_t r1 = sd_cmd(55, 0);
    if (r1 & 0x80u) return r1;
    return sd_cmd(41, arg);
}

/* Standard SPI-mode bring-up: CMD0 -> CMD8 (SDv2 probe) -> ACMD41 loop
 * -> CMD58 (SDv2, CCS bit) or CMD16 (SDv1/MMC, force 512-byte blocks).
 * Sets sd_is_v2/sd_block_addressed on success. Returns true on
 * success, leaving clock at SD_NORMAL_BAUD_HZ, CS deasserted. */
static bool sd_do_init(void) {
    uint8_t r1;
    uint8_t rx[4], tx[4];
    int i, j;
    bool is_v2 = false;

    spi_set_baudrate(SPI_PORT, SD_INIT_BAUD_HZ);
    apply_mode();
    cs_deassert();

    for (j = 0; j < 4; j++) tx[j] = 0xFFu;
    for (i = 0; i < 3; i++) {
        spi_write_read_blocking(SPI_PORT, tx, rx, 4);   // >=74 dummy clocks total, CS high (3*4*8=96 clocks)
    }

    cs_assert();
    r1 = 0xFFu;
    for (i = 0; i < 10; i++) {
        r1 = sd_cmd(0, 0);
        if (r1 == 0x01u) break;
    }
    if (r1 != 0x01u) {
        if (debug_enabled) debug_printf("SD: CMD0 failed, r1=0x%02X (no card / not responding)\n", r1);
        cs_deassert();
        return false;
    }

    r1 = sd_cmd(8, 0x1AAul);
    if (r1 == 0x01u) {
        uint8_t r7[4];
        for (j = 0; j < 4; j++) tx[j] = 0xFFu;
        spi_write_read_blocking(SPI_PORT, tx, rx, 4);
        for (j = 0; j < 4; j++) r7[j] = rx[j];
        if (r7[2] != 0x01u || r7[3] != 0xAAu) {
            debug_printf("SD: CMD8 pattern mismatch -- unsupported card\n");
            cs_deassert();
            return false;
        }
        is_v2 = true;
    } else if (r1 & 0x04u) {
        is_v2 = false;   // illegal command -- SDv1 or MMC
    } else {
        debug_printf("SD: CMD8 unexpected r1=0x%02X\n", r1);
        cs_deassert();
        return false;
    }

    for (i = 0; i < 1000; i++) {
        r1 = sd_acmd41(is_v2 ? 0x40000000ul : 0ul);
        if (r1 == 0x00u) break;
        if (r1 != 0x01u) {
            debug_printf("SD: ACMD41 unexpected r1=0x%02X\n", r1);
            cs_deassert();
            return false;
        }
    }
    if (r1 != 0x00u) {
        debug_printf("SD: ACMD41 never left idle -- timeout\n");
        cs_deassert();
        return false;
    }

    if (is_v2) {
        r1 = sd_cmd(58, 0);
        if (r1 != 0x00u) {
            debug_printf("SD: CMD58 unexpected r1=0x%02X\n", r1);
            cs_deassert();
            return false;
        }
        for (j = 0; j < 4; j++) tx[j] = 0xFFu;
        spi_write_read_blocking(SPI_PORT, tx, rx, 4);
        sd_block_addressed = (rx[0] & 0x40u) ? true : false;
        if (!sd_block_addressed) {
            r1 = sd_cmd(16, 512ul);
            if (r1 != 0x00u) {
                debug_printf("SD: CMD16 (set blocklen 512) failed, r1=0x%02X\n", r1);
                cs_deassert();
                return false;
            }
        }
    } else {
        r1 = sd_cmd(16, 512ul);
        if (r1 != 0x00u) {
            debug_printf("SD: CMD16 (set blocklen 512) failed, r1=0x%02X\n", r1);
            cs_deassert();
            return false;
        }
        sd_block_addressed = false;
    }

    cs_deassert();
    spi_set_baudrate(SPI_PORT, SD_NORMAL_BAUD_HZ);
    apply_mode();

    sd_is_v2 = is_v2;
    if (debug_enabled) {
        debug_printf("SD: init ok (%s, %s-addressed)\n",
               sd_is_v2 ? "SDv2" : "SDv1/MMC",
               sd_block_addressed ? "block" : "byte");
    }
    return true;
}

static uint32_t sd_arg_for_lba(uint32_t lba) {
    return sd_block_addressed ? lba : (lba * 512u);
}

/* CMD13 SEND_STATUS -- returns a 2-byte R2 response in SPI mode (R1 followed
 * by 2nd status byte). Used here purely as a "card is present and talking" probe. */
static bool sd_check_media(void) {
    uint8_t r1, r2;

    if (!sd_initialized) return false;

    cs_assert();
    r1 = sd_cmd(13, 0);
    r2 = spi_byte(0xFFu);   // read 2nd byte of R2 response so card finishes CMD13
    cs_deassert();          // deasserts CS and sends dummy clocks to release MISO
    (void)r2;               // only used to complete the R2 transaction, not part of the check
    // A real "card responded normally" check, not just "bit 7 clear" --
    // CMD13's R1 byte should be exactly 0x00 for a healthy, initialized
    // card; anything else (illegal command, CRC error, address error,
    // etc.) should count as a failed media check even though several of
    // those error bits don't set bit 7.
    return r1 == 0x00u;
}

/* CMD17 READ_SINGLE_BLOCK into out[0..511]. Returns true on success. */
static bool sd_read_one_sector(uint32_t lba, uint8_t *out) {
    uint8_t r1, token;
    uint8_t tx[512], discard[2];
    int i;

    cs_assert();
    r1 = sd_cmd(17, sd_arg_for_lba(lba));
    if (r1 != 0x00u) {
        debug_printf("SD: CMD17 lba=%lu r1=0x%02X\n", (unsigned long)lba, r1);
        cs_deassert();
        return false;
    }

    token = 0xFFu;
    for (i = 0; i < 8000; i++) {
        token = spi_byte(0xFFu);
        if (token != 0xFFu) break;
    }
    if (token != 0xFEu) {
        debug_printf("SD: CMD17 lba=%lu bad data token 0x%02X\n", (unsigned long)lba, token);
        cs_deassert();
        return false;
    }

    for (i = 0; i < 512; i++) tx[i] = 0xFFu;
    spi_write_read_blocking(SPI_PORT, tx, out, 512);
    tx[0] = 0xFFu; tx[1] = 0xFFu;
    spi_write_read_blocking(SPI_PORT, tx, discard, 2);   // trailing CRC, discarded

    cs_deassert();
    return true;
}

/* CMD24 WRITE_BLOCK from in[0..511]. Returns true on success. */
static bool sd_write_one_sector(uint32_t lba, const uint8_t *in) {
    uint8_t r1, resp, token, crc[2], discard[2];
    int i;

    cs_assert();
    r1 = sd_cmd(24, sd_arg_for_lba(lba));
    if (r1 != 0x00u) {
        debug_printf("SD: CMD24 lba=%lu r1=0x%02X\n", (unsigned long)lba, r1);
        cs_deassert();
        return false;
    }

    token = 0xFEu;
    spi_write_read_blocking(SPI_PORT, &token, &resp, 1);
    spi_write_read_blocking(SPI_PORT, (uint8_t *)in, sd_write_discard, 512);
    crc[0] = 0xFFu; crc[1] = 0xFFu;
    spi_write_read_blocking(SPI_PORT, crc, discard, 2);

    resp = spi_byte(0xFFu);
    if ((resp & 0x1Fu) != 0x05u) {
        debug_printf("SD: CMD24 lba=%lu data rejected, resp=0x%02X\n", (unsigned long)lba, resp);
        cs_deassert();
        return false;
    }

    // Busy-wait: card holds MISO low while programming. Can legitimately
    // take a couple hundred ms -- see TRINKET.C's sd_write_block() for
    // the same wait, whose loop count this mirrors.
    //
    // 2026-09-14 fix: this used to unconditionally `return true` right
    // after this loop, regardless of whether the card actually signaled
    // done (a non-0x00 byte) or the loop simply exhausted all 60000
    // iterations still seeing 0x00 (card still busy, or stuck/faulted).
    // That meant a write taking longer than this budget to finish
    // committing was reported to the host as a successful write. Now
    // distinguishes the two and reports failure on a genuine timeout,
    // instead of silently claiming success on data that might not have
    // actually landed.
    for (i = 0; i < 60000; i++)
        if (spi_byte(0xFFu) != 0x00u) break;

    if (i >= 60000) {
        debug_printf("SD: CMD24 lba=%lu write-busy timeout, card never signaled done\n", (unsigned long)lba);
        cs_deassert();
        return false;
    }

    cs_deassert();
    return true;
}

// ---------------------------------------------------------------------
// Write-direction: host is issuing a new SD operation.
// ---------------------------------------------------------------------

static bool recv_next_byte(uint8_t *out) {
    if (!wire_wait_init_high()) {
        return false;
    }
    return wire_finish_write_handshake(out);
}

static bool handle_op_init(void) {
    bool ok = sd_do_init();
    sd_initialized = ok;
    sd_last_status = ok ? SD_STATUS_OK : SD_STATUS_ERROR;
    sd_last_len = 0;
    return true;
}

static bool handle_op_read_sectors(void) {
    uint8_t b[5];
    uint32_t lba;
    uint8_t count;
    int i;

    for (i = 0; i < 5; i++)
        if (!recv_next_byte(&b[i])) return false;

    lba = (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
    count = b[4];

    if (count > SD_MAX_SECTORS) {
        debug_printf("SD: READ_SECTORS count=%u exceeds SD_MAX_SECTORS=%u -- rejected\n",
               (unsigned)count, (unsigned)SD_MAX_SECTORS);
        sd_last_status = SD_STATUS_ERROR;
        sd_last_len = 0;
        return true;
    }

    for (i = 0; i < count; i++) {
        if (!sd_read_one_sector(lba + (uint32_t)i, &sd_buf[(size_t)i * 512])) {
            sd_last_status = SD_STATUS_ERROR;
            sd_last_len = 0;
            return true;
        }
        subsys_dac_service();
    }

    sd_last_status = SD_STATUS_OK;
    sd_last_len = (uint16_t)((uint32_t)count * 512u);
    if (debug_enabled) debug_printf("SD: READ_SECTORS lba=%lu count=%u ok\n", (unsigned long)lba, (unsigned)count);
    return true;
}

static bool handle_op_write_sectors(void) {
    uint8_t b[5];
    uint32_t lba;
    uint8_t count;
    uint16_t total, i;
    bool oversize;

    for (i = 0; i < 5; i++)
        if (!recv_next_byte(&b[i])) return false;

    lba = (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
    count = b[4];
    oversize = (count > SD_MAX_SECTORS);
    total = (uint16_t)((uint32_t)count * 512u);

    // Every payload byte the host committed to sending has to be
    // drained via a full write handshake regardless of oversize, same
    // "stay in lockstep" rule as subsys_spi.c's handle_op_xfer() --
    // otherwise the next dispatch cycle desyncs.
    for (i = 0; i < total; i++) {
        uint8_t byte;
        if (!recv_next_byte(&byte)) return false;
        if (!oversize) sd_buf[i] = byte;
    }

    if (oversize) {
        debug_printf("SD: WRITE_SECTORS count=%u exceeds SD_MAX_SECTORS=%u -- rejected\n",
               (unsigned)count, (unsigned)SD_MAX_SECTORS);
        sd_last_status = SD_STATUS_ERROR;
        sd_last_len = 0;
        return true;
    }

    for (i = 0; i < count; i++) {
        if (!sd_write_one_sector(lba + (uint32_t)i, &sd_buf[(size_t)i * 512])) {
            sd_last_status = SD_STATUS_ERROR;
            sd_last_len = 0;
            return true;
        }
        subsys_dac_service();
    }

    sd_last_status = SD_STATUS_OK;
    sd_last_len = 0;
    if (debug_enabled) debug_printf("SD: WRITE_SECTORS lba=%lu count=%u ok\n", (unsigned long)lba, (unsigned)count);
    return true;
}

static bool handle_op_check_media(void) {
    bool ok = sd_check_media();
    sd_last_status = ok ? SD_STATUS_OK : SD_STATUS_ERROR;
    sd_last_len = 0;
    if (debug_enabled) {
        debug_printf("SD: CHECK_MEDIA %s\n", ok ? "card responding" : "no response / not initialized");
    }
    return true;
}

static bool run_write_side(void) {
    uint8_t opcode;

    if (!wire_finish_write_handshake(&opcode)) {
        return false;
    }

    switch (opcode) {
        case SD_OP_INIT:
            return handle_op_init();
        case SD_OP_READ_SECTORS:
            return handle_op_read_sectors();
        case SD_OP_WRITE_SECTORS:
            return handle_op_write_sectors();
        case SD_OP_CHECK_MEDIA:
            return handle_op_check_media();
        default:
            debug_printf("SD: unknown op 0x%02X -- ignoring\n", opcode);
            return true;
    }
}

// ---------------------------------------------------------------------
// Read-direction: host wants the result of the last completed SD_OP_*.
// See subsys_sd.h's header comment for the exact framing.
// ---------------------------------------------------------------------

static bool send_next_byte(uint8_t byte) {
    uint8_t corrected = byte ^ 0x88u;                          // doc sec 7 -- see subsys_spi.c's header comment on this correction
    uint8_t low_nibble  = corrected & 0x0Fu;
    uint8_t high_nibble = (corrected >> 4) & 0x0Fu;

    if (!wire_finish_read_cycle(low_nibble, high_nibble)) return false;
    if (!wire_wait_init_high()) return false;
    return wire_finish_read_cycle(low_nibble, high_nibble);
}

static bool send_subsequent_byte(uint8_t byte) {
    if (!wire_wait_init_high()) return false;
    return send_next_byte(byte);
}

static bool run_read_side(void) {
    if (!send_next_byte(sd_last_status)) return false;
    if (!send_subsequent_byte(sd_is_v2 ? 1u : 0u)) return false;
    if (!send_subsequent_byte(sd_block_addressed ? 1u : 0u)) return false;
    if (!send_subsequent_byte((uint8_t)(sd_last_len & 0xFFu))) return false;
    if (!send_subsequent_byte((uint8_t)((sd_last_len >> 8) & 0xFFu))) return false;

    for (uint16_t i = 0; i < sd_last_len; i++) {
        if (!send_subsequent_byte(sd_buf[i])) return false;
    }

    if (debug_enabled) {
        debug_printf("SD: read result status=%u len=%u\n", (unsigned)sd_last_status, (unsigned)sd_last_len);
    }
    return true;
}

bool subsys_sd_run(void) {
    if (!wire_wait_init_high()) {
        return false;
    }

    if (wire_is_write_selected()) {
        return run_write_side();
    }
    return run_read_side();
}

void subsys_sd_dump_diagnostics(void) {
    printf("[SD / SPI Subsystem Status (SPI0 on H3 Header)]\n");
    printf("  GPIO0 (MISO/RX):  %u (func %u)\n", gpio_get(PIN_SPI_MISO), gpio_get_function(PIN_SPI_MISO));
    printf("  GPIO1 (CS):       %u (manual CS: %s)\n", gpio_get(PIN_SPI_CS), gpio_get(PIN_SPI_CS) ? "DEASSERTED" : "ASSERTED (active-low)");
    printf("  GPIO2 (SCK):      %u (func %u)\n", gpio_get(PIN_SPI_SCK), gpio_get_function(PIN_SPI_SCK));
    printf("  GPIO3 (MOSI/TX):  %u (func %u)\n", gpio_get(PIN_SPI_MOSI), gpio_get_function(PIN_SPI_MOSI));
    printf("  Card Initialized: %s\n", sd_initialized ? "YES" : "NO");
    if (sd_initialized) {
        printf("  Card Specs:       %s, %s\n",
               sd_is_v2 ? "SD v2 (or higher)" : "SD v1",
               sd_block_addressed ? "Block-addressed (SDHC/SDXC)" : "Byte-addressed (SDSC)");
    }
    printf("  Last Transaction: Status=%s (0x%02X), Payload=%u bytes\n",
           (sd_last_status == SD_STATUS_OK) ? "OK" : "ERROR", sd_last_status, (unsigned)sd_last_len);
}
