# TRINKET.SYS

One concrete `CONFIG.SYS DEVICE=` block-device driver for a DOS-era SD
card reader built on the trinket parallel-port bridge. No TSR, no INT
2Fh redirector, no separate hardware-layer driver + MSCDEX-analog pair
-- DOS sees a normal block device with a BPB, the same way it sees a
floppy or hard disk, and every FAT16 filesystem operation (dir, copy,
etc.) is handled by DOS itself against that BPB. This directory is the
whole driver, three source files plus the two skeleton files DOS itself
requires.

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

  Comparing sdpp's SDMM.C against TRINKET.C revealed why the crash at 039F:192C / 039F:193C was occurring:

  1. SDMM.C explicitly defined #define NOSHIFT:
  In Borland C 3.1 16-bit small model (-ms), performing 32-bit shifts or 32-bit multiplications on DWORD values (such as arg >> 24 or
  lba * 512ul) forces the compiler to link external runtime assembly helper routines from cs.lib (LXURSH@ / H_LURSH and LXLSH@ /
  H_LLSH).
  2. The Stack Mismatch:
  Because TRINKET.C performed 32-bit shifts in sd_cmd (arg >> 24, arg >> 16, arg >> 8) and sd_arg_for_lba (lba * 512ul), tlink pulled
  H_LURSH (LXURSH@) into the binary right at the end of the _TEXT segment (offset 192C / 193C).
  When sd_cmd called LXURSH@ via a near call, LXURSH@ executed its far return (RETF), corrupting the return stack frame and causing
  execution to fall off the end of _TEXT into _DATA (hitting Exception 06 #UD at 039F:0000193C).
  3. Stack Frame Depth:
  TRINKET.C's spi_byte -> spi_xfer -> spi_get_result stack allocation chain created deep stack frames on every single byte transfer on
  HEADER.ASM's 256-byte stack.

  ### Changes Made to TRINKET.C

  1. Adopted SDMM.C's #define NOSHIFT byte-indexing in sd_cmd:
  Replaced 32-bit right shifts with direct Little-Endian byte pointer access:
    frame[1] = ((BYTE *)&arg)[3];
    frame[2] = ((BYTE *)&arg)[2];
    frame[3] = ((BYTE *)&arg)[1];
    frame[4] = ((BYTE *)&arg)[0];
    
  2. Shift-Free LBA Address Calculation in sd_arg_for_lba:
  Replaced 32-bit multiplication lba * 512ul with 16-bit WORD shifts:
    static DWORD sd_arg_for_lba(DWORD lba)
    {
       DWORD res;
       WORD lo, hi;
       if (g_block_addressed) return lba;
       lo = ((WORD *)&lba)[0];
       hi = ((WORD *)&lba)[1];
       ((WORD *)&res)[0] = (lo << 9);
       ((WORD *)&res)[1] = (lo >> 7) | (hi << 9);
       return res;
    }

  3. Preserved Stack Memory:
      • Changed local temporary arrays/buffers in spi_byte, spi_xfer, spi_op_cs, spi_op_config, and spi_get_result to static storage
      class to prevent stack allocation churn on the 256-byte stack.
      • Changed wait_error_high and wait_error_low loop indices from 32-bit DWORD to 16-bit UINT.


  ### Verification

  • H_LURSH (LXURSH@) and H_LLSH (LXLSH@) are completely gone from bin/trinket.map.
  • Recompiled clean with make (0 errors, 0 warnings).
  • src/HEADER.ASM, src/DRIVER.C, src/CPRINT.C, and src/SD.C remain 100% original / unchanged.

  Additionally, the driver loads, maps to D: and then provides accsess to the sd card.

