# TRINKET.SYS

One concrete `CONFIG.SYS DEVICE=` block-device driver for a DOS-era SD
card reader built on the trinket parallel-port bridge. No TSR, no INT
2Fh redirector, no separate hardware-layer driver + MSCDEX-analog pair
-- DOS sees a normal block device with a BPB, the same way it sees a
floppy or hard disk, and every FAT16 filesystem operation (dir, copy,
etc.) is handled by DOS itself against that BPB. This directory is the
whole driver, three source files plus the two skeleton files DOS itself
requires.

## Firmware vs DOS driver

This repo intentionally contains two separate toolchains and code paths:

- The DOS driver source in `src/` is the real-mode Borland C/Turbo Assembler path that produces `TRINKET.SYS` for a DOS-era machine.
- The Pico firmware source in `firmware/` is a separately-built CMake project for the RP2040 and uses the Raspberry Pi Pico SDK instead of the DOS toolchain.

They share the same design concepts and wire protocol, but they are not built from the same toolchain, and they are not meant to be compiled together. The firmware work is treated as a separate subsystem validation target and a future host-side block-device implementation path, while the DOS code remains the actual runtime driver for the old PC environment.

For the Pico-side build and validation workflow, see [firmware/README.md](./firmware/README.md).

## Where each piece came from
The original Borland C++ repo for PPSD at: https://github.com/nilseuropa/sdpp. For this build we simply stuck to 
a functional workflow and replaced the wire backend to handle the link requests though the new paralell read/write
trinket calls.  This saves a huge amount of time bit-banging SPI through the port.

| File | Origin | Changed? |
|---|---|---|
| `HEADER.ASM` | `sdpp-main` (R. Armstrong, 1994) | No -- device header/STRATEGY/INTERRUPT glue, no SD-specific code in it at all |
| `DRIVER.C` | `sdpp-main` | Banner text only -- INIT/READ/WRITE/IOCTL/BPB dispatch untouched |
| `SD.C` | `sdpp-main` | No -- FAT16 BPB/MBR partition discovery, pure sector-level logic, backend-agnostic |
| `CPRINT.C/H`, `STANDARD.H`, `DRIVER.H`, `DISKIO.H`, `INTEGER.H` | `sdpp-main` | No |
| `TRINKET.C` | **new** | Replaces `sdpp-main`'s `SDMM.C` (bit-banged SPI). Implements the same `diskio.h` contract, but talks to the trinket's wire/dispatch/SPI protocol instead of toggling SPI on raw parallel-port data lines. |

`SD.C` calling `disk_read()`/`disk_write()`/`disk_initialize()`/
`disk_status()` is the seam that made this possible without touching
DOS-facing code at all: everything above that line (BPB math, MBR
partition scan, DOS request-header dispatch) doesn't know or care how a
sector actually gets fetched.

## Where `TRINKET.C` came from

Ported, not reinvented, from two already-exercised pieces of
`trinket_ifacetest`:

- **Wire-level handshake** (`ctrl_out`/`io_delay`/`wait_error_high`/
  `wait_error_low`/`raw_send_byte`/`raw_recv_byte`/`host_reset`) --
  line-for-line from `driver/backend.c`, which is the host-side half of
  the protocol **confirmed on real hardware** (`TRINKET.SYS`
  character-device build, 2026-08-13 per that project's `NOTES.md`).
  Only the asm idiom changed: `backend.c` is GCC extended asm
  (`ia16-elf-gcc`); this uses Borland/Turbo C's `outp()`/`inp()`
  (`<conio.h>`) -- the same convention `sdpp-main`'s own `SDMM.C`
  already used for parallel-port I/O, so no inline asm was needed at
  all at this layer.

- **DISPATCH_CMD_SPI framing + SD-in-SPI-mode command layer**
  (`spi_op_config`/`spi_op_cs`/`spi_xfer`/`spi_byte`, `sd_init`/
  `sd_cmd`/`sd_acmd41`/`sd_read_block`/`sd_write_block`) -- ported from
  `client/sdtest.c`, which per this project's own memory is the one
  piece of this whole stack **already confirmed end-to-end against real
  media**: SDv2 card init, FAT16 mount, directory listing, and file
  reads, all via that standalone diagnostic EXE.

## What's genuinely new and unconfirmed here

- **The dispatch/SPI subsystem itself, device-side** (`subsys_spi.c` on
  the trinket firmware). Per `trinket_ifacetest/README.md`: written and
  only compile-checked against stub SDK headers, never built with a
  real `arm-none-eabi-gcc`/`pico-sdk`, never run against real SPI
  hardware.
- **Driving all of this from inside a resident, DOS-request-triggered
  `.SYS` module** rather than a human running one `SDTEST.EXE` command
  at a time. The logic is identical to `sdtest.c`; what's untested is
  whether it behaves the same when `disk_read()`/`disk_write()` call it
  back-to-back across many DOS-generated sector requests, in the
  segment/stack environment a real device driver runs in (see
  `sdpp-main`'s own HEADER.ASM comments on the SMALL-model CS/DS
  concerns that motivated most of its assembly layer).
- **The Borland-C port of the wire/SPI layer itself.** The logic is a
  direct port of hardware-confirmed (wire) and media-confirmed (SD
  command layer) C, but it has not itself been compiled with `bcc`/
  `tasm` or run on real hardware yet -- see "Suggested bring-up order"
  below for how to isolate a problem if one shows up.

## What was deliberately dropped from `sdpp-main`

- **Card-detect** (`/k`, `CDDETECT()`). The trinket's GPIO map has no
  line dedicated to SD card-detect over this link -- `sdpp-main`'s
  CD-on-BUSY wiring doesn't apply. `/k` is accepted on the
  `CONFIG.SYS` line and silently ignored.
- **Multi-block read/write** (`CMD18`/`CMD25`). `sdtest.c` never had
  it either -- `disk_read()`/`disk_write()` here just loop the
  single-block `CMD17`/`CMD24` commands sector by sector. Correct, not
  fast: per `sdtest.c`'s own header comment, every SPI byte costs two
  full dispatch round trips over the parallel link, so large transfers
  will be slow until/unless that's revisited.

## CONFIG.SYS usage

```
DEVICE=TRINKET.SYS /p=<partition #> /b=<port base index>
```

Same option letters as `sdpp-main`, `/k` accepted but inert (see
above):

```
/d = debugging mode (verbose cdprintf messages, incl. TRINKET.C's own
     CMD0/CMD8/ACMD41/CMD17/CMD24 diagnostics on failure)
/p = partition number (1-4) in the MBR to use. Default: first available.
/b = port base index: 1=0x3BC, 2=0x378, 3=0x278, 4=0x3E8, 5=0x2E8
     Default: 0x378
```
## Prebuilt Driver
The toolchain to build the driver is specific and antiqated.  Prebuilt binary can
be found in the `dist` directory.


## Build

This project requires Borland C++ 3.1 and Turbo Assembler 3.1, identical to the 
toolchain used by `sdpp-main`. These proprietary tools are not included; if you 
have a licensed copy, place them in `./legacy/bc31`.

You will also need dosemu2 to run the buildchain in linux with the defualt `Makefile`.
This will asume that dosemu2 brings up your c drive as `~/.dosemu/drive_c` and copy 
the build directory there so that the defualt `./src/MAKEFILE` (borland) does not break.

please see https://github.com/dosemu2/dosemu2/issues/1145 for dosemu2 setup.

if you set up the above you can run: 
```
make 
```
produces `./bin/trinket.sys`.

## Build on actual DOS notes:
You will want to properly set up Borland C++ 3.1 using its standard configuration, be sure to set the
following in your autoexec.bat: 
```
SET INCLUDE=C:\BC31\INCLUDE\
SET LIB=C:\BC31\LIB
SET TEMP=C:\TEMP

PATH C:\;C:\BC31\BIN;%PATH%
```

`BUILD.BAT` will also try to set these if you just run it from wherever you've set it.

## Functional Mockup:
### Key Discovery in SDMM.C vs TRINKET.C

The DOS-era SD driver path had two hard real-mode issues that had to be fixed before it could boot cleanly:

1. **32-bit shift/multiply helpers were being pulled in from cs.lib**
  In Borland C 3.1 16-bit small model (-ms), performing operations like
  `arg >> 24`, `arg >> 16`, or `lba * 512ul` on DWORD values can force the
  compiler to emit calls to external runtime helpers such as `LXURSH@`/
  `H_LURSH` and `LXLSH@`/`H_LLSH` from `cs.lib`.

2. **The far pointer bug**
  `BYTE DOSFAR *p` arithmetic like `p += 512` only changes the 16-bit
  offset portion of the far pointer. When the pointer is near the end of a
  64KB segment, the offset wraps back to zero instead of carrying into the
  next segment. That corrupts memory during multi-sector reads/writes.

The fix was implemented in `src/TRINKET.C` by replacing the 32-bit shift
based code with byte-index extraction and by adding a segment-aware
`advance_far_ptr()` helper:

```c
static BYTE DOSFAR *advance_far_ptr(BYTE DOSFAR *p, WORD add_bytes)
{
   WORD seg = FP_SEG(p);
   WORD off = FP_OFF(p);
   WORD new_off = off + add_bytes;
   WORD new_seg = seg + ((new_off < off) ? 4096 : 0);
   return (BYTE DOSFAR *) MK_FP(new_seg, new_off);
}
```

This is used in `disk_read()` and `disk_write()` to advance across the
next 64KB segment instead of wrapping inside the current one.

### Changes Made to TRINKET.C

1. **Shift-free SD command framing**
  Replaced 32-bit right shifts in `sd_cmd()` with direct byte access:

  ```c
  frame[1] = ((BYTE *)&arg)[3];
  frame[2] = ((BYTE *)&arg)[2];
  frame[3] = ((BYTE *)&arg)[1];
  frame[4] = ((BYTE *)&arg)[0];
  ```

2. **Shift-free LBA math**
  Replaced `lba * 512ul` with a 16-bit word-based calculation that stays in
  small-model-safe arithmetic.

3. **Stack-preserving changes**
  - moved temporary data to static storage where the 256-byte DOS stack
    could not absorb large transient arrays;
  - reduced loop counters to 16-bit `UINT` where appropriate.

### Verification

- `H_LURSH` / `H_LLSH` are absent from the link map.
- The DOS build compiles cleanly.
- `src/HEADER.ASM`, `src/DRIVER.C`, `src/CPRINT.C`, and `src/SD.C` remain
  original and unchanged.
- The driver boots, maps to a drive letter, and provides access to the SD
  card.

