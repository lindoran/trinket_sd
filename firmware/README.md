## Trinket Test Suite
This is the foundational c only trinket firmware testing suite.

---

### What this is:
This serves as two functions: first, it provides a basis for how the drivers will be written going forward in C, with linked assembly only where needed; second, it provides a test harness to validate the device behavior on real hardware.

This directory is intentionally separate from the DOS driver code in `../src/` because it targets a different runtime and toolchain. The DOS driver builds under Borland C++/Turbo Assembler for a 16-bit real-mode environment, while the firmware here builds for the RP2040 Pico using CMake and the Pico SDK. The wire protocol is shared, but the build environment, target architecture, and validation flow are not.

---

### Firmware build scripts
The firmware repo includes three convenience scripts at the top level:

- `setup-firmware.sh`: ensures the Pico SDK, ARM toolchain, and `picotool` are available under `~/.pico-sdk` and links the installed tools into the expected locations. This is the bootstrap step if the SDK/toolchain is not already installed.
- `build-firmware.sh`: runs the setup step, configures CMake for the firmware tree, and builds the RP2040 firmware. (The host-side client tools in `firmware/client` build separately now, via `build-client.sh` at the repo root -- Borland C++ 3.1 under dosbox, not part of this firmware toolchain. See the top-level TOOLCHAIN.md.)
- `clean-firmware.sh`: removes the generated `firmware/build` directory.

Typical flow:

```bash
./setup-firmware.sh
./build-firmware.sh
```

The normal output is a UF2 image under `firmware/build/`. For the
host-side client binaries (`SDTEST.EXE`/`IFTEST.EXE`), run the
repo-root `build-client.sh` instead.

under linux, you can manually build the whole repo with a full chain 
using `make` and `make clean`.  This performs as expected, and runs
the correct scripts.  You can also do `make package` to zip the 
deliverables.

---

---

### What is working:
The reset path is confirmed working end-to-end, and the current firmware
has moved the SD logic onto the Pico itself.

- `src/wire.c`: GPIO13 (nAuto-LF) is a host-driven reset line with a GPIO
  IRQ. Asserting it aborts the in-flight handshake, parks the Pico's
  outputs at a known idle state, waits for the line release, then clears
  per-subsystem state before the main loop resumes. This is the same
  protocol reset pattern the host-side `host_reset()` in `client/*` uses.
- `client/iftest.c` and `client/sdtest.c` both call `host_reset()` at
  the start of a standalone run to ensure a known starting state.
- The Pico firmware is now a real command dispatcher built around
  `src/dispatch.c` / `src/dispatch.h`, with a test-harness subsystem and
  a dedicated SD subsystem.
- `src/subsys_sd.c` / `src/subsys_sd.h` implement the high-level SD-over-
  SPI protocol: `SD_OP_INIT`, `SD_OP_READ_SECTORS`, `SD_OP_WRITE_SECTORS`,
  and `SD_OP_CHECK_MEDIA`.
- The host-side `fast*` commands in `client/sdtest.c` (`fastinit`,
  `fastcheck`, `fastread`, `fastwrite`, `fastbench`) exercise that
  firmware path and are the current validation target for the SD
  offload design.

This is the important distinction: the project is no longer a pure
single-file echo test. The current design is “wire protocol + dispatch +
subsystems,” and the SD subsystem is the first full firmware-side
implementation of the original enhancement plan.

The current firmware build is confirmed to compile cleanly with the real
Pico SDK toolchain via the project build scripts. The remaining work is
mostly validation against real media and cycle tuning, not a design gap in
whether the subsystem model is correct.

---

### Current status of the original enhancement plan:

1. **High-level firmware offloading**: implemented in `src/subsys_sd.c` and
   exercised by the `fast*` client commands.
2. **Multi-block SD transfers (`CMD18`/`CMD25`)**: not implemented yet; the
   firmware still loops per sector.
3. **Media-check / VolID caching**: the firmware-side `SD_OP_CHECK_MEDIA`
   prefilter exists; host-side media-change caching is still a separate
   follow-up.
4. **SPP handshake tuning**: present in C (`io_delay`, `host_reset`,
   `raw_send_byte`, `raw_recv_byte`) in the host-side test harness; the
   hand-written assembly unroll described in the enhancement document is
   not present in this checkout.
5. **Far-pointer normalization**: implemented in the legacy DOS-side path
   in `src/TRINKET.C` via `advance_far_ptr()`, not in the firmware tree.

---

### Next validation steps:

1. **Regression check on the SD reset bug**: confirm `fastcheck` works
   after a host reset without clearing the card's initialized state.
2. **Firmware hardware verification**: run the `fast*` commands against a
   real SD card and compare behavior to the older host-side `bench` path.
3. **Measure/compare the optimized path**: verify the firmware-offloaded
   `fastbench` is actually faster and consistent relative to the original
   single-block read loop.
4. **Extend to multi-block writes/reads**: when ready, move `CMD18` and
   `CMD25` into the SD subsystem as the next performance step.

This is the active branch of the project now: use the dispatch model,
verify the SD subsystem against real hardware, and only then add the next
performance layer.

---

### Hardware Overview

| Ref | Part | Function | Direction |
|---|---|---|---|
| U1 | RP2040 (Pico) | Main MCU | — |
| U2 | 4-channel level shifter | Control-line inputs, LPT→Pico | 5V (LPT side, 10K pull-ups) → 3.3V (Pico side) |
| U3 | 74AHCT245 | Status-line outputs, Pico→LPT | 3.3V (Pico side) → 5V (LPT side), permanently direction-selected |
| U4 | 74LVC245 | Data bus, LPT→Pico | 5V (LPT side) → 3.3V (Pico side), permanently direction-selected |

**Headers:**
- **H1** — I2C, 3.3V, 4.7K pull-ups (GPIO26/27)
- **H2** — UART/I2C, 3.3V, no pull-ups on prototype (GPIO16/17)
- **H3** — SPI, SD card interface (GPIO0–3)

---

### GPIO Map

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
| 16 | — | — | — | — | H2 | TX / repurposable I2C, no pull-ups on prototype |
| 17 | — | — | — | — | H2 | RX / repurposable I2C, no pull-ups on prototype |
| 18 | Busy | 11 | 11 | Pico→LPT | U3 | Status bit 7 (**hw inverted**) |
| 19 | nAck | 10 | 10 | Pico→LPT | U3 | Status bit 6 |
| 20 | Paper-Out | 12 | 12 | Pico→LPT | U3 | Status bit 5 |
| 21 | Select | 13 | 13 | Pico→LPT | U3 | Status bit 4 |
| 22 | nError | 15 | 32 | Pico→LPT | U3 | Status bit 3 |
| 26 | SDA | — | — | — | H1 | I2C, 4.7K @ 3.3V |
| 27 | SCL | — | — | — | H1 | I2C, 4.7K @ 3.3V |

**Note on "hw inverted":** this inversion happens inside the PC's parallel port ASIC, between the Control/Status register bit and the physical pin. It is entirely a host-driver concern. The trinket firmware only ever sees/drives the raw physical pin level and does **not** need to account for it.

### Host-side register inversion table (for `.sys` driver reference)

| Signal | Register bit | Bit=1 → physical pin | Bit=0 → physical pin |
|---|---|---|---|
| nStrobe | Control 0 | LOW | HIGH |
| nAuto-LF | Control 1 | LOW | HIGH |
| nInit | Control 2 | **HIGH** (not inverted) | LOW |
| nSelect-In | Control 3 | LOW | HIGH |
| Busy | Status 7 | reads 0 when pin=HIGH | reads 1 when pin=LOW |
| (all other status bits) | Status 6,5,4,3 | not inverted — bit value = pin level | |

---