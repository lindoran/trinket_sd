# Parallel Port (SPP) Driver & Firmware Enhancement Suggestions

This document outlines proposed architecture, performance, and stability enhancements for the **Trinket Parallel-Port-to-SD** driver (`TRINKET.SYS`) and RP2040 firmware. 

These recommendations specifically target **Standard Parallel Port (SPP)** mode (using 8-bit output data bus `REG_DATA` and 4-bit nibble status input `REG_STATUS`).

---

## 1. High-Level Firmware Offloading (High Impact)

### The Problem
Currently, [TRINKET.C](src/TRINKET.C) manages the entire SD protocol over the parallel port via `DISPATCH_CMD_SPI`. To issue an SD command (`CMD0`, `CMD8`, `ACMD41`, `CMD17`) or poll MISO for responses/tokens (`0xFE`), DOS sends framing commands over LPT and performs individual byte exchanges.

In SPP mode, every byte fetched from the device requires **2 nibble handshakes** (4 control port writes, multiple status port reads, and ISA bus delays). Polling the card's MISO byte-by-byte over LPT generates thousands of round-trips over the slow parallel bus just waiting for tokens.

### Suggested Enhancement
Create a dedicated **SD Subsystem (`DISPATCH_CMD_SD`)** in the RP2040 firmware ([subsys_spi.c](firmware/src/subsys_spi.c)):
- **Move Card Init & Protocol Logic to RP2040**: Have the RP2040 execute `CMD0`, `CMD8`, `ACMD41`, `CMD16/CMD58`, R1 parsing, and token waiting directly on the RP2040 (running at 125MHz with hardware SPI).
- **High-Level LPT Commands**:
  - `SD_CMD_INIT` $\rightarrow$ RP2040 initializes card, detects SDv1/v2/SDHC, and returns status in 1 byte over LPT.
  - `SD_CMD_READ_SECTORS(lba, count)` $\rightarrow$ RP2040 reads SD sectors directly via SPI into a buffer and streams raw 512-byte payload blocks over LPT.
  - `SD_CMD_WRITE_SECTORS(lba, count)` $\rightarrow$ DOS streams raw payload blocks over LPT; RP2040 writes directly to SPI.
- **Benefit**: Eliminates ~90% of parallel-port handshake traffic and framing overhead.

---

## 2. Multi-Sector & SD Multi-Block Transfers (`CMD18` / `CMD25`)

### The Problem
In [TRINKET.C](src/TRINKET.C), `disk_read` and `disk_write` loop single-block operations (`CMD17`/`CMD24`) sector-by-sector. When the FreeDOS kernel (`blockio.c`) requests a cluster read (e.g., 16 to 64 sectors), DOS toggles CS high/low and sends new `CMD17` commands for *every single 512-byte sector*.

### Suggested Enhancement
- Implement **Multi-Block Read (`CMD18`)** and **Multi-Block Write (`CMD25`)** on the RP2040 firmware.
- Allow DOS to pass `count` directly to a multi-sector transfer command (`SD_CMD_READ_SECTORS`).
- The RP2040 holds Chip Select (CS) low, issues `CMD18`, and continuously streams payload sectors over SPP without re-issuing SD commands between blocks, issuing `CMD12` (STOP_TRANSMISSION) when finished.

---

## 3. Media Check & Caching Optimizations

### The Problem
In FreeDOS, commands issued by FreeCOM or applications trigger a `MEDIA CHECK` (Request 1 in FreeDOS kernel `device.h`) before file reads.
Currently, `SDMediaCheck` in [src/SD.C](src/SD.C) executes `disk_read(unit, local_buffer, 0, 1)`—reading Sector 0 (512 bytes) off the SD card over LPT **on every media check call** to verify `BS_VolID`. This adds significant latency to basic operations like `DIR`.

### Suggested Enhancement
- **Firmware-based Fast Status Check**: Expose a 1-byte LPT status command (`SD_CMD_CHECK_MEDIA`) where the RP2040 reports whether SPI line errors or card disconnects occurred.
- **Host-Side VolID Caching**: In [src/SD.C](src/SD.C), store the last read timestamp using `bios_ticks()`. If a media check occurs within a short interval (e.g., < 1-2 seconds), return `M_NOT_CHANGED` (1) immediately without re-reading Sector 0 off the SD card.

---

## 4. SPP Wire Protocol & Handshake Tuning (Host Driver)

### The Problem
In [src/TRINKET.C](src/TRINKET.C), `io_delay()` performs 4 consecutive reads of port `0x80` per handshake cycle. In SPP nibble mode, fetching 512 bytes performs 1024 nibble transfers (thousands of port `0x80` reads).

### Suggested Enhancement
- **Unrolled Bulk Nibble Stream**: Write a specialized inline assembly loop for 512-byte sector reads in `TRINKET.C` (`spp_read_sector_fast`). Keeping DX loaded with `base_port + REG_STATUS` / `REG_CONTROL` avoids C function call overhead per byte.
- **Configurable `io_delay()`**: Make port `0x80` bus delays configurable (e.g., reducing from 4 reads to 1 or 2 reads on faster 486/Pentium systems where parallel port signals settle quickly).

---

## 5. DOS Real-Mode Far Pointer Segment Normalization

### The Problem
In 16-bit real-mode DOS drivers, DOS passes data buffers as far pointers (`BYTE far *buff`).
In [src/TRINKET.C](src/TRINKET.C), incrementing a far pointer (`p += 512`) only modifies the 16-bit offset. If DOS passes a DTA buffer located near the end of a segment (e.g., offset `0xFE00`), `p += 512` wraps to `0x0000` within the *same segment*, corrupting memory during multi-sector reads.

### Suggested Enhancement
Normalize segment/offset pointers during multi-sector increments in `TRINKET.C`:
```c
static BYTE far* normalize_far_ptr(BYTE far *p, WORD add_bytes) {
    DWORD linear = (((DWORD)FP_SEG(p)) << 4) + FP_OFF(p) + add_bytes;
    return (BYTE far*) MK_FP((WORD)(linear >> 4), (WORD)(linear & 0x0F));
}
```

---

## Current Repo Status (as implemented today)

The project has already moved the main SD logic onto the RP2040 firmware and added the fast-path `fast*` client checks. The original enhancement list still describes the conceptual next steps, but the code in this repo reflects the following actual status:

- **1. High-Level Firmware Offloading**: implemented in `firmware/src/subsys_sd.*` and exercised by the `fastinit` / `fastcheck` / `fastbench` client commands in `firmware/client/sdtest.c`.
- **2. Multi-Block Reads/Writes (`CMD18` / `CMD25`)**: not implemented in the current firmware; the implementation still loops sector-by-sector.
- **3. Media Check / VolID Caching**: firmware-side `SD_OP_CHECK_MEDIA` is implemented; host-side VolID caching is still a separate follow-up and not yet in this repo.
- **4. SPP Wire Protocol Tuning**: the low-level nibble handshakes are implemented in C (`io_delay`, `host_reset`, `raw_send_byte`, `raw_recv_byte`); the fully hand-written assembly unroll described in the proposal is not present.
- **5. Far Pointer Normalization**: implemented in the legacy DOS-side code path, in `src/TRINKET.C` via `advance_far_ptr()`. The fix is used by `disk_read()` / `disk_write()` and carries segment wraparound across 64KB boundaries instead of just incrementing the offset.

## Summary of Enhancements

| Enhancement | Target Area | Status | Notes |
| :--- | :--- | :--- | :--- |
| **High-Level SD Subsystem** | RP2040 Firmware & client test harness | **Implemented** | SD protocol logic now lives in `firmware/src/subsys_sd.c`; host sends high-level ops and the RP2040 executes the real SD sequence. |
| **Multi-Block Reads (`CMD18`)** | RP2040 Firmware | **Planned / Not yet implemented** | Current code still performs per-sector loops. |
| **VolID / Media Check Caching** | `SD.C` / host driver layer | **Partial** | `SD_OP_CHECK_MEDIA` firmware check is present; host-side media-change caching is still separate from the current repo. |
| **Unrolled SPP Nibble Assembly** | `TRINKET.C` / raw client path | **Partially implemented** | Same protocol tuning exists in C in `firmware/client/sdtest.c`; no hand-written assembly unroll has been added here. |
| **Far Pointer Normalization** | Legacy DOS-side driver path | **Implemented** | The fix is in `src/TRINKET.C` (`advance_far_ptr()`), not in the firmware tree. |
