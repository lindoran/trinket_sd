# Firmware and DOS tools

The RP2040 firmware (`src/`), the DOS-side diagnostic and configuration
tools (`client/`), and host-side unit tests (`tests/`). The DOS driver lives
in `../src/` and builds with a different toolchain; the two sides share a wire
protocol but are never built together. For build instructions see
[../TOOLCHAIN.md](../TOOLCHAIN.md); for using the result see
[../README.md](../README.md).

## Build

From the repository root: `./build-firmware.sh` (firmware only, ->
`dist/trinket_sd.uf2`), `./build-client.sh` (DOS tools, needs Borland C++ 3.1),
or `./build.sh` for everything. `./clean-firmware.sh` removes
`firmware/build/` and `dist/trinket_sd.uf2`. Flash the UF2 by holding BOOTSEL
while connecting the Pico's USB port and copying it to the `RPI-RP2` drive.

Host-side unit tests (no hardware, just `gcc`): `firmware/tests/run_tests.sh`
runs the Disney Sound Source FIFO model and its configuration protocol.

## Architecture

**Wire protocol and dispatch.** The PC and the trinket talk over the SPP
parallel port using a byte handshake: writes (PC to trinket) move one byte per
handshake, reads (trinket to PC) move a nibble per handshake, so reads cost
about twice the handshakes per byte. `wire.c` is that layer; `dispatch.c`
reads a command byte and hands the transaction to a subsystem. Each operation
is two phases: a write-direction cycle submits it and a separate
read-direction cycle fetches the result.

| Command | Subsystem | Source | Purpose |
|---|---|---|---|
| `0x01` | Test harness | `subsys_testharness.c` | Echo test, used by `IFTEST` |
| `0x02` | Raw SPI bridge | `subsys_spi.c` | Byte-level SPI, used by `SDTEST`'s original commands |
| `0x03` | High-level SD | `subsys_sd.c` | `INIT`, `READ_SECTORS`, `WRITE_SECTORS`, `CHECK_MEDIA`. All SD command sequencing (CMD0/8/ACMD41/58/16/17/24/13) runs on the RP2040, so a whole multi-sector request is one round trip |
| `0x04` | PCM5102 DAC | `subsys_dac.c`, `audio_i2s.pio` | PIO + DMA I2S, 16 KB ring buffer, internal-PLL or 256*Fs external clock, 8- and 16-bit PCM, built-in tone synthesis |
| `0x05` | Sound Source configuration | `subsys_dss.c`, `dss_cmd.c` | FIFO depth and warm-up. See [../DSS.md](../DSS.md) |

**Sound Source mode.** Separate from the dispatcher: when the port's control
lines show the Disney Sound Source signature (pins 14 and 16 both high),
`main.c` runs `subsys_dss_run_session()` (`dss_capture.pio`, `dss_model.c`)
instead of the dispatcher, feeding the same DAC ring. Details in
[../DSS.md](../DSS.md).

**Reset.** GPIO13 (nAuto-LF) is a host-driven reset line with an interrupt.
Asserting it aborts the in-flight handshake, parks the outputs at idle, waits
for release and clears per-subsystem state. The wire layer's waits time out
(`WIRE_TIMEOUT_US`, 250 ms) so an incomplete handshake cannot hang the Pico,
except the top-level wait for a new command, which is unbounded by design.

**SD notes.** Multi-block commands (`CMD18`/`CMD25`) are not implemented; the
firmware loops single-block `CMD17`/`CMD24` internally. A write whose
post-write busy-wait times out is reported as a failure, not a success.

**Console.** USB CDC serial. ESC prints diagnostics, toggles verbose logging,
resets all subsystems and plays a short beep (serviced between commands only).
The on-board LED pulses on dispatch activity.

## DOS tools (`client/`)

`iftest.c`, `sdtest.c`, `dactest.c`, `dsstest.c`, `dssstres.c`, `dsscfg.c`:
plain real-mode `.EXE`s that drive the port directly, with no driver loaded.
Usage for each is in its source header and in [../README.md](../README.md).
`sdtest.c` also contains the `fast*` commands that exercise the firmware's SD
offload, the standalone way to test the RP2040 side without `TRINKET.SYS`.
File names are DOS 8.3-safe on purpose.

## Hardware overview

| Ref | Part | Function | Direction |
|---|---|---|---|
| U1 | RP2040 (Pico) | Main MCU | — |
| U2 | 4-channel level shifter | Control-line inputs, LPT→Pico | 5V (LPT side, 10K pull-ups) → 3.3V (Pico side) |
| U3 | 74AHCT245 | Status-line outputs, Pico→LPT | 3.3V (Pico side) → 5V (LPT side), permanently direction-selected |
| U4 | 74LVC245 | Data bus, LPT→Pico | 5V (LPT side) → 3.3V (Pico side), permanently direction-selected |

**Headers:**
- **H1** — I2C-style header, 3.3V, GPIO26/27. Pull-ups removed on the prototype so the PCM5102 DAC connects directly (see the DAC wiring below)
- **H2** — UART/I2C, 3.3V, no pull-ups on prototype (GPIO16/17)
- **H3** — SPI, SD card interface (GPIO0–3)

---

## GPIO map

| GPIO | Signal | LPT Pin (D-25) | Centronics Pin | Direction | Chip | Register / Bit |
|---|---|---|---|---|---|---|
| 0 | MISO | — | — | — | H3 | SD SPI |
| 1 | SS | — | — | — | H3 | SD SPI |
| 2 | SCK | — | — | — | H3 | SD SPI |
| 3 | MOSI | — | — | — | H3 | SD SPI |
| 4–11 | D0–D7 | 2–9 | 2–9 | LPT→Pico | U4 | Data Register, bits 0–7 |
| 12 | nStrobe | 1 | 1 | LPT→Pico | U2 | Control bit 0 (**hw inverted**) |
| 13 | nAuto-LF | 14 | 14 | LPT→Pico | U2 | Control bit 1 (**hw inverted**) |
| 14 | nInit | 16 | 31 | LPT→Pico | U2 | Control bit 2 (not inverted) |
| 15 | nSelect-In | 17 | 36 | LPT→Pico | U2 | Control bit 3 (**hw inverted**) |
| 16 | BCK (Bit Clock) | — | — | Pico→DAC | H2 | PCM5102 I2S Bit Clock |
| 17 | LRCK (WS) | — | — | Pico→DAC | H2 | PCM5102 I2S Left/Right Clock |
| 18 | Busy | 11 | 11 | Pico→LPT | U3 | Status bit 7 (**hw inverted**) |
| 19 | nAck | 10 | 10 | Pico→LPT | U3 | Status bit 6 |
| 20 | Paper-Out | 12 | 12 | Pico→LPT | U3 | Status bit 5 |
| 21 | Select | 13 | 13 | Pico→LPT | U3 | Status bit 4 |
| 22 | nError | 15 | 32 | Pico→LPT | U3 | Status bit 3 |
| 26 | SCLK (MCLK) | — | — | Pico→DAC | H1 | PCM5102 Master Clock (Internal PLL: LOW / External: PWM) |
| 27 | DIN (Data In) | — | — | Pico→DAC | H1 | PCM5102 I2S Serial Data |

**Note on "hw inverted":** this inversion happens inside the PC's parallel port ASIC, between the Control/Status register bit and the physical pin. It is entirely a host-driver concern. The trinket firmware only ever sees/drives the raw physical pin level and does **not** need to account for it.

**PCM5102 DAC wiring:** GPIO16 = BCK, GPIO17 = LRCK, GPIO27 = DIN, GPIO26 = SCLK (low selects the module's internal PLL, a PWM 256*Fs clock selects the external clock), plus 3.3 V and GND from the Pico. On the prototype GPIO26/27 have no pull-ups and the module's solder bridges are all left open.

## Host-side register inversion table (for `.sys` driver reference)

| Signal | Register bit | Bit=1 → physical pin | Bit=0 → physical pin |
|---|---|---|---|
| nStrobe | Control 0 | LOW | HIGH |
| nAuto-LF | Control 1 | LOW | HIGH |
| nInit | Control 2 | **HIGH** (not inverted) | LOW |
| nSelect-In | Control 3 | LOW | HIGH |
| Busy | Status 7 | reads 0 when pin=HIGH | reads 1 when pin=LOW |
| (all other status bits) | Status 6,5,4,3 | not inverted — bit value = pin level | |

---