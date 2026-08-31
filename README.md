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
| `DRIVER.C` | `sdpp-main` | Banner text, far-pointer normalization, and bios_ticks export |
| `SD.C` | `sdpp-main` | MediaCheck optimized with firmware CMD13 prefilter + VolID time caching |
| `CPRINT.C/H`, `STANDARD.H`, `INTEGER.H` | `sdpp-main` | No |
| `DRIVER.H`, `DISKIO.H` | `sdpp-main` | Added prototypes (bios_ticks, disk_check_media) |
| `TRINKET.C` | **new** | Speaks to trinket's high-level SD subsystem (DISPATCH_CMD_SD) with raw_recv_bulk streaming, offloading SD protocol to RP2040 |

`SD.C` calling `disk_read()`/`disk_write()`/`disk_initialize()`/
`disk_status()`/`disk_check_media()` is the seam that made this possible:
everything above that line (BPB math, MBR partition scan, DOS request-header
dispatch) operates cleanly against the high-level block interface.

## Where `TRINKET.C` came from

Ported and refined from `trinket_ifacetest` and `firmware/`:

- **Wire-level handshake** (`ctrl_out`/`io_delay`/`wait_error_high`/
  `wait_error_low`/`raw_send_byte`/`raw_recv_byte`/`raw_recv_bulk`/`host_reset`) --
  hardware-confirmed wire protocol with streaming bulk nibble reads.
- **High-Level SD Subsystem** (`DISPATCH_CMD_SD` / `SD_OP_INIT` / `SD_OP_READ_SECTORS` /
  `SD_OP_WRITE_SECTORS` / `SD_OP_CHECK_MEDIA`) -- protocol logic offloaded to RP2040
  firmware, cutting parallel port round trips by >90%.

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
   seg += (off >> 4);
   off = (off & 0x0F) + add_bytes;
   seg += (off >> 4);
   off &= 0x0F;
   return (BYTE DOSFAR *) MK_FP(seg, off);
}
```

This is used in `disk_read()` and `disk_write()` (as well as `DRIVER.C`'s `ReadBlock()`/`WriteBlock()`) to advance across 64KB segments safely instead of wrapping within the current segment.

### Changes Made to the DOS Driver

1. **High-Level SD Subsystem (`DISPATCH_CMD_SD`)**
   Offloaded SD protocol negotiation, sector reading, writing, and presence checks onto the RP2040 firmware, eliminating per-byte parallel port dispatch overhead.

2. **Streaming Bulk Handshake (`raw_recv_bulk`)**
   Implemented bulk nibble read loops in `TRINKET.C` directly into far buffers with segment-normalized pointer stepping.

3. **Fast Media Check & VolID Caching**
   `SDMediaCheck()` combines a fast firmware status probe (`SD_OP_CHECK_MEDIA` / CMD13) with `bios_ticks()` timestamp caching to prevent redundant Sector 0 reads during frequent DOS calls like `DIR`.

4. **Normalized Far Pointer Arithmetic**
   Uses 16-bit word arithmetic exclusively to normalize far pointers on multi-sector increments, avoiding segment wraparound and removing all 32-bit `cs.lib` dependencies.

### Verification

- `H_LURSH` / `H_LLSH` / `N_LXLSH@` / `N_LXURSH@` are absent from the link map.
- The DOS build compiles and links cleanly with zero warnings/errors.
- The firmware and client test suite compile cleanly.

