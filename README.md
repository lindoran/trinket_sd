# TRINKET SD (what is it?)
This is a one stop shop SPP based sound and storage option for dos era
gear that could benifit from it existing.   This will let you bring up 
a mass storage device, play sounds and maybe more.. all from a standard
lpt port.

Why? specifically there's a need because projects like the much simpler
sdpp project exist but are flawed.  Having a very standard way to 
connect a modern serial device to a paralell port without having to 
worry if the card is going to be compatible, or if the port wiring is 
supported by the tranciever on the sd card module.   And while this 
isn't fast -- its a fair bit faster for bakcking up data to a card, 
as the computer no longer has to worry about i2c timing, it just sends 
bytes down the wire.

# TRINKET.SYS

One concrete `CONFIG.SYS DEVICE=` block-device driver for a DOS-era SD
card reader built on the trinket parallel-port bridge. No TSR, no INT
2Fh redirector, no separate hardware-layer driver + MSCDEX-analog pair
-- DOS sees a normal block device with a BPB, the same way it sees a
floppy or hard disk, and every FAT16 filesystem operation (dir, copy,
etc.) is handled by DOS itself against that BPB.

**Status**: working end to end on real hardware -- `TRINKET.SYS` boots
DOS from an SD card and has run Wolfenstein 3D from it, reported by feel
as no slower than a parallel-port Zip drive. See "Where things stand"
below for what that does and doesn't cover, and "Where this is headed"
for what's next -- notably, further storage-side optimization is
intentionally paused (see that section).

## Repo layout

Two separate toolchains and code paths live here, sharing wire
protocols but never built together:

- **`src/`** -- the real-mode Borland C++ 3.1/Turbo Assembler driver
  that produces `TRINKET.SYS` for a DOS-era machine.
- **`firmware/`** -- the RP2040 side: a CMake/Pico-SDK project
  (`firmware/src/`) plus DOS-side diagnostic client tools
  (`firmware/client/`, e.g. `sdtest.c`'s `fastinit`/`fastread`/
  `fastbench`/`fastcheck` commands -- the standalone way to exercise the
  RP2040 side without the driver in the loop at all). The client tools
  build with the same Borland C++ 3.1 toolchain as `src/` (as of
  2026-09-08 -- they used to be a third, ia16-elf-gcc toolchain; see
  TOOLCHAIN.md for why that changed), so this project has two toolchains
  total, not three: arm-none-eabi-gcc for `firmware/src/`, Borland C++
  3.1 for everything DOS-side (`src/` and `firmware/client/` alike).

See [firmware/README.md](./firmware/README.md) for the firmware side in
its own detail, and **[TOOLCHAIN.md](./TOOLCHAIN.md) for the full,
current build-environment story** -- what each toolchain actually needs,
what reproduces in a fresh sandbox and what doesn't, and the real
hazards and build-system gotchas a clean build alone won't catch. Both
are referenced throughout this file rather than duplicated.

## Building

```
./build.sh            # builds firmware + DOS driver + DOS client tools, -> dist/
./clean-all.sh
./check-toolchain.sh  # check to see if the build depends exist.

```

The same build chain is available through the top-level `Makefile`:

```
make                  # build firmware, DOS driver, and DOS client tools
make package          # build everything and create .tar.gz, .7z, and .zip archives
make toolchain        # check all build/package dependencies
make firmware         # build RP2040 firmware only
make client           # build only the DOS client tools (Borland C++ 3.1 + dosbox)
make driver           # build only the DOS driver (Borland C++ 3.1 + dosbox)
make setup            # install or link firmware build dependencies
make clean            # remove all generated artifacts
make help             # list all targets
```

Or individually: `build-firmware.sh`/`clean-firmware.sh` (RP2040 only),
`build-driver.sh`/`clean-driver.sh` (the DOS driver), and
`build-client.sh`/`clean-client.sh` (the DOS client tools) -- the latter
two both via dosbox + the vendored Borland toolchain, no dosemu2 needed.
All six leave their output in `dist/`.

**One thing you must supply yourself**: `legacy/bc31/` (Borland C++
3.1) is gitignored on purpose -- it's proprietary software this repo
has no right to redistribute, so a plain clone/zip export will never
include it. `build-driver.sh` and `build-client.sh` both fail with a
clear message explaining this if it's missing; see TOOLCHAIN.md for
exactly what needs to be there.

## Where each piece came from

The original Borland C++ repo for PPSD:
https://github.com/nilseuropa/sdpp. This project kept its functional
workflow (DOS-side FAT16/BPB/MBR logic, `diskio.h` contract) and
replaced the storage backend to talk to the trinket link instead of
bit-banging SPI directly on parallel-port data lines.

| File | Origin | Changed? |
|---|---|---|
| `HEADER.ASM` | `sdpp-main` (R. Armstrong, 1994) | No -- device header/STRATEGY/INTERRUPT glue, no SD-specific code |
| `DRIVER.C` | `sdpp-main` | Banner text; added `bios_ticks()` and its own `advance_far_ptr()` for Debug-mode timing and far-pointer safety in `ReadBlock()`/`WriteBlock()`; INIT/READ/WRITE/IOCTL/BPB dispatch shape otherwise untouched |
| `SD.C` | `sdpp-main` | `SDMediaCheck()` now does a real VolID comparison, a firmware-side `CHECK_MEDIA` pre-filter, and a short time-based debounce (see below); FAT16 BPB/MBR partition discovery itself is unchanged |
| `CPRINT.C/H`, `STANDARD.H`, `INTEGER.H` | `sdpp-main` | No |
| `DRIVER.H`, `DISKIO.H` | `sdpp-main` | One new prototype each (`bios_ticks()`, `disk_check_media()`) |
| `TRINKET.C` | **new** | Replaces `sdpp-main`'s `SDMM.C` (bit-banged SPI). Speaks the trinket's high-level `DISPATCH_CMD_SD` protocol -- sends a sector number and a count, gets sector data back. Doesn't itself know about CMD17/CMD24/R1/data-tokens/byte-vs-block-addressing any more; that all runs on the RP2040 now (`firmware/src/subsys_sd.c`) |

`SD.C` calling `disk_read()`/`disk_write()`/`disk_initialize()`/
`disk_status()`/`disk_check_media()` is the seam that made this
possible without touching DOS-facing code: everything above that line
(BPB math, MBR partition scan, DOS request-header dispatch) doesn't
know or care how a sector actually gets fetched.

`TRINKET.C` itself was ported, not reinvented, in two pieces:

- **Wire-level handshake** (`ctrl_out`/`io_delay`/`wait_error_high`/
  `wait_error_low`/`raw_send_byte`/`raw_recv_byte`/`host_reset`, plus
  `raw_recv_bulk_far`/`raw_send_bulk_far` for far-pointer-safe
  multi-byte transfers) -- confirmed on real hardware; uses Borland/
  Turbo C's `outp()`/`inp()` (`<conio.h>`) rather than inline asm, the
  same convention `sdpp-main`'s own `SDMM.C` already used.
- **`DISPATCH_CMD_SD` client layer** (`sd_op_init`/`sd_op_read_sectors`/
  `sd_op_write_sectors`/`sd_op_check_media`) -- ported from
  `firmware/client/sdtest.c`'s own `sd2_*` client, itself confirmed via
  the standalone `fastinit`/`fastread`/`fastwrite`/`fastbench`/
  `fastcheck` diagnostic commands before this driver ever used it.

## Where things stand

**Confirmed on real hardware, end to end**: card init, sector read/write,
media check, and DOS booting + running software (Wolf3D) from the card
through this exact driver -- not just isolated benchmarks.

**Deliberately not implemented**:
- **Card-detect** (`/k`). The trinket's GPIO map has no card-detect
  line -- `sdpp-main`'s CD-on-BUSY wiring doesn't apply here. `/k` is
  accepted on the `CONFIG.SYS` line and silently ignored.
- **Multi-block SD commands** (`CMD18`/`CMD25`). `disk_read()`/
  `disk_write()` already batch a whole multi-sector request into one
  `DISPATCH_CMD_SD` round trip, but the RP2040 firmware still loops
  single-block `CMD17`/`CMD24` internally rather than issuing a real
  multi-block command. On hold -- see "Where this is headed" below.

**Worth knowing about, not bugs**:
- `SDMediaCheck()`'s time-based debounce skips even the cheap
  `CHECK_MEDIA` probe if checked within the last ~2 seconds -- a real,
  if narrow, window where a physical card swap wouldn't be detected.
  Probably fine given a swap takes a human several seconds minimum, but
  it's a deliberate trade against the driver's original always-verify
  guarantee, not a free win.
- The wire protocol's own per-byte handshake, not SD-command overhead,
  dominates the remaining per-block time: moving SD command sequencing
  onto the RP2040 (this section's whole `DISPATCH_CMD_SD` migration)
  measured as a real but modest ~26% faster than the old per-sector
  path, not the dramatic win a naive read of the numbers might suggest.
  The lever that would actually move that floor is a wire-protocol-level
  redesign (batch the acknowledgment every N bytes instead of every
  byte) -- see below for why that's paused too.

## Where this is headed

The next planned addition is **audio** -- likely starting with something
Disney-Sound-Source-shaped (a raw PCM stream, no synthesis) through a
PCM5102 I2S DAC on the RP2040's spare GPIOs, with the RP2040's PIO+DMA
generating the I2S bitstream. Whether that becomes transparently
compatible with existing DOS games' native Adlib/Disney-Sound-Source
port I/O, or needs its own explicit client software, is still an open
design question -- the parallel port here is already fully committed to
`DISPATCH_CMD_SD`'s framing, so a second device sharing that same wire
needs either a second physical LPT port, a V86 port-trapping TSR, or a
firmware-side hardware snoop on a second port's electrical lines. The
audio pipeline itself (PCM5102 wiring, PIO/DMA streaming, a bespoke
`DISPATCH_CMD_SOUND` test command in the sdtest.c style) is the planned
first concrete step, independent of that question.

**Storage-side optimization is intentionally on hold until the sound
work is solid.** Specifically: multi-block SD commands (`CMD18`/`CMD25`)
and any wire-protocol-level burst-transfer redesign are paused, not
abandoned. Both would touch the same dispatch/wire-protocol layer a
second, sound-capable device will also need to share -- doing that
redesign once, informed by both storage's and sound's actual
requirements, is the goal; doing it now for storage alone would risk
having to revisit or partially undo it once sound's requirements are
known. `DISPATCH_CMD_SD` in its current form is considered a stable
foundation to build the sound work on top of, not a placeholder that's
about to change shape.

## CONFIG.SYS usage

```
DEVICE=TRINKET.SYS /p=<partition #> /b=<port base index>
```

Same option letters as `sdpp-main`, `/k` accepted but inert (see above):

```
/d = debugging mode -- DRIVER.C's own verbose cdprintf messages (media
     check calls, init/read/write status, Debug-mode timing breakdown).
     TRINKET.C itself always prints one unconditional line on a
     successful init ("card ready (...)"); the detailed CMD0/CMD8/
     ACMD41/CMD17/CMD24-level diagnostics that used to live here now run
     on the trinket's own RP2040 firmware (its own USB-CDC debug log,
     toggled independently -- see firmware/README.md), not gated by
     this flag at all.
/p = partition number (1-4) in the MBR to use. Default: first available.
/b = port base index: 1=0x3BC, 2=0x378, 3=0x278, 4=0x3E8, 5=0x2E8
     Default: 0x378
```

## Real-mode hazards this driver had to work around

Two hard real-mode issues had to be solved before this could boot
cleanly, and both recur any time new arithmetic touches this driver's
`STRATEGY`/`INTERRUPT` call path. **See
[TOOLCHAIN.md](./TOOLCHAIN.md#known-driver-specific-hazards) for the
canonical writeup** (including a variant of the second one -- far
pointer *array indexing*, not just `+=`/`++` -- found later during the
`DISPATCH_CMD_SD` migration). Short version:

1. **32-bit shift/multiply pulls in `cs.lib` helpers**
   (`arg >> 24`, `lba * 512ul`-style `DWORD` operations compile to calls
   into `N_LXLSH@`/`N_LXURSH@`), which crash with exception 06 from this
   driver's private stack. Fixed by keeping this arithmetic in 16-bit
   (`WORD`) pieces -- byte-reinterpretation for command arguments,
   direct-memory-write construction for `bios_ticks()`.
2. **Far pointer arithmetic only touches the 16-bit offset** (`p += N`,
   `p++`, and array indexing `p[i]` all have this problem identically) --
   it doesn't carry into the segment, so a buffer positioned near a 64K
   boundary silently corrupts. Fixed by walking a local far pointer via
   `advance_far_ptr()` instead. `TRINKET.C` and `DRIVER.C` each carry
   their own independently-written (both verified correct) version of
   this helper -- a carry-on-wraparound approach in `TRINKET.C`, a
   two-pass minimal-offset normalization in `DRIVER.C`.

**Verification, checked directly rather than assumed**: `H_LURSH`/
`H_LLSH`/`N_LX*` are absent from both the link map and `bcc -S` output
(re-checked after the `DISPATCH_CMD_SD` migration and the media-check
rewrite); the DOS build compiles and links cleanly via `build-driver.sh`;
the driver boots, maps to a drive letter, and has run DOS software from
the card end to end.

## Building on real DOS hardware

If you're building directly on a period DOS machine rather than
cross-building via `build-driver.sh`, set up Borland C++ 3.1 the
standard way in `autoexec.bat`:

```
SET INCLUDE=C:\BC31\INCLUDE\
SET LIB=C:\BC31\LIB
SET TEMP=C:\TEMP
PATH C:\;C:\BC31\BIN;%PATH%
```

Then `cd src` and run the vendored `MAKEFILE` (`make -f MAKEFILE`)
directly.
