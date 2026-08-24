## Trinket Test Suite
This is the foundational c only trinket firmware testing suite.

---

### What this is:
This serves as two functions, one this provides a basis for how the drivers will be written going forward, in C using linked asembly were needed.  Two this provides a test harness to asure the device works.

---

### What is working:
This loads, and fires off the test from the drvtest.exe perfectly. Reset
is implemented end to end and **confirmed working on real hardware**:

- `src/wire.c`: GPIO13 (nAuto-LF) is a host-driven reset line with a GPIO
  IRQ. Asserting it (control bit1=0 -> physical pin HIGH) aborts whatever
  handshake is in progress, parks the Pico's outputs at a known-idle
  level, and blocks until the line is released (back LOW) before the
  main loop clears every subsystem's own state and resumes -- see
  `src/wire.c`'s `wire_do_reset_sequence()` for the exact detect/wait/
  clear sequence.
- `client/ifacetest.c` has its own `host_reset()` (a direct port-I/O
  copy of `backend_reset()`), called unconditionally at the start of
  `main()`, so the standalone no-driver diagnostic gets the same
  known-starting-state guarantee.


Also new: the Pico firmware is no longer a single-file echo test. It's
now a small **command dispatcher** (`src/dispatch.c`/`.h`) sitting in
front of the same proven wire-level plumbing (`src/wire.c`/`.h`, split
out unchanged), with the original echo behavior living behind it as the
first subsystem (`src/subsys_testharness.c`/`.h`). Every transfer on the
wire now starts with an 8-bit command byte selecting which subsystem
handles it -- `driver/backend.c` injects that command byte
automatically on every `backend_write_byte()`/`backend_read_byte()`
call, so `TRINKET.SYS` and anything that talks to it (including
`client/drvtest.c`) get this for free without any changes on their end.
`client/ifacetest.c`, which bypasses the driver, sends the command byte
itself. Both currently only ever send `DISPATCH_CMD_TEST_HARNESS`. See
`driver/NOTES.md`'s "Dispatch" section for the full design writeup.

There are now two subsystems: `DISPATCH_CMD_TEST_HARNESS` (unchanged)
and `DISPATCH_CMD_SPI` (`src/subsys_spi.c`/`.h`), a basic SPI master
running on the H3 header's real SPI0 hardware (GPIO0-3) -- not
SD-specific, just "configure rate/mode, move CS, run a full-duplex
transfer, read the result back." See `subsys_spi.h`'s header comment
for the exact request/response framing. `TRINKET.SYS`/`driver/backend.c`
don't talk to it yet -- per its own scope note, `TRINKET.SYS` stays a
plain byte pipe; a future block-device TSR is expected to drive
`DISPATCH_CMD_SPI` directly (its own dispatch-byte injection, same
pattern `backend.c` already uses for the test harness), not through
`TRINKET.SYS`'s existing `CMD_INPUT`/`CMD_OUTPUT` semantics.

**Not yet tested on hardware, and not yet built at all** -- the
wire-level plumbing this was built from was confirmed working as a
single file; the split into wire.c/dispatch.c/subsys_testharness.c and
the dispatch framing itself are new and unverified on real hardware.
`subsys_spi.c` is newer still: checked by inspection and by compiling
against hand-written stub SDK headers (no `arm-none-eabi-gcc`/vendored
`pico-sdk` in the environment it was written in), never built against
the real SDK, never run against real SPI hardware. See `driver/NOTES.md`.

---

### When implementing the next phase:
Reset is done, the dispatcher has two subsystems (test harness, SPI),
and `client/sdtest.c` is a first cut at proving SD-over-SPI + FAT16
sector I/O standalone (no driver, no TSR -- see its own header comment).
The next real gaps, in rough order:

1. **Confirm the dispatcher on real hardware** -- device side
   (`src/dispatch.c` + the wire.c split) and host side (`backend.c`'s
   injected command byte, `ifacetest.c`'s explicit one) both need a
   real run before either is trusted the way reset now is. Still true,
   unchanged by SPI's addition.
2. **Build and hardware-test `subsys_spi.c`.** Written and only
   compile-checked against stub SDK headers -- needs a real
   `arm-none-eabi-gcc`/`pico-sdk` build, then a logic-analyzer check on
   GPIO0-3 (clock rate/mode/CS timing against `SPI_OP_CONFIG`/
   `SPI_OP_CS`/`SPI_OP_XFER`) before pointing it at any real device.
3. **Build and hardware-test `client/sdtest.c`** against a real SD
   card, once (2) is confirmed -- `init`/`dir`/`cat`/`readsector`/
   `writesector`. This is where the SD-in-SPI-mode command sequencing
   (CMD0/CMD8/ACMD41/CMD16/CMD58, block read/write) and the FAT16 BPB/
   root-dir/cluster-chain parsing get proven against real media, before
   any of that logic moves into a resident TSR.
4. **The block-device TSR itself.** Per discussion: full DOS drive
   letter via the INT 2Fh network-redirector interface (the real
   MSCDEX mechanism), not a CONFIG.SYS block device driver -- avoids a
   reboot on every iteration, which a `.SYS` load doesn't. Talks to
   trinket the same way `sdtest.c` does (own raw port I/O, own
   `DISPATCH_CMD_SPI` injection, bypassing `TRINKET.SYS`), reusing
   `sdtest.c`'s SD/FAT16 logic once it's proven rather than
   re-deriving it inside the TSR from scratch.
5. Per `driver/NOTES.md`'s original scope note, any of that is meant to
   be layered on top of the now-proven plumbing here (a new subsystem
   module, a new `backend_*`-shaped entry point calling a new dispatch
   command), not a rewrite of `entry.S`/`link.ld`.

See `docs/TESTING_CHECKLIST.md` for the ordered, concrete version of
(1)-(3) above -- what to actually run, in what order, and what to
compare it against. See `docs/CODEGEN_NOTES.md` if you're an LLM (or a
human) about to add more code in this style -- it covers a comment-
syntax footgun this repo's style makes easy to hit, plus what "checked
by inspection" has actually meant in practice so far.

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