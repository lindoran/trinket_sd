# Alternate bootstrap: copy `TRINKET.SYS` without the driver

These are hand-entry bootstrap loaders for the case where DOS cannot already
see the Trinket SD card as a drive. They do **not** install or replace
firmware, a DOS driver, or an existing utility.

## What exists here

| File | Use it when | Size |
|---|---|---|
| [`TRINKET.BAS`](./TRINKET.BAS) | Normal choice: QBasic, GW-BASIC. | ~4.5 KB, 101 lines |
| [`TRINKSML.BAS`](./TRINKSML.BAS) | Fallback for a very old BASIC (for example an early BASICA) that stops with `Out of string space`. | ~4.2 KB, 100 lines |

Both are line-numbered, direct-port copiers. Each uses the firmware's SD sector
command, finds the root-directory `TRINKET.SYS` entry, follows its FAT16
cluster chain, and writes the file to `A:TRINKET.SYS`. Edit `P=888` for another
LPT base address and change `A:` (lines 40, 360, 370 and 540) if the
writable target is another drive. No line is longer than 96 characters.

How they differ:

- `TRINKET.BAS` keeps the 512-byte sector in four 128-byte strings (GW-BASIC
  and BASICA strings cannot exceed 255 characters) and writes the output in
  128-byte random-access records. The file is therefore **padded with zero
  bytes to a multiple of 128** (9,599 bytes becomes 9,600). Trailing zeros
  after a device driver are expected to be harmless.
- `TRINKSML.BAS` keeps the sector in an integer array, so it needs almost no
  string space, and writes one byte per record, so the output is the **exact
  file size**. The price is speed: one `PUT` per byte is noticeably slower,
  especially to a floppy.

Type the lines into BASIC (or `LOAD` the ASCII file), save the program in RAM or
on the target disk, and run it with the Trinket connected. It overwrites
`A:TRINKET.SYS`; use a writable, formatted disk with enough free space. On
success, add `DEVICE=A:TRINKET.SYS` to `CONFIG.SYS` and reboot.

The `.BAS` files use CRLF line endings. If you transfer them as text, keep it
that way; LF-only files are not recommended. If you type them in, save with
`SAVE "TRINKET",A` (GW-BASIC/BASICA) or the normal QBasic save.

The program assumes one of the supported FAT16 layouts: either a superfloppy
FAT16 volume or a FAT16 primary partition (partition types 04h, 06h, or 0Eh)
in the MBR. It expects 512-byte sectors and the 8.3 root entry `TRINKET.SYS`.
It reads the file's actual directory size and follows the FAT chain, so the
file need not be contiguous. FAT32, FAT12, extended/logical partitions,
nonstandard sector sizes, and subdirectory copies are not supported. Sector
numbers are held in single-precision BASIC variables, so LBAs above
16,777,215 (beyond 8 GB) are not handled.

## Compatibility

| Interpreter | Status |
|---|---|
| QBasic 1.0 and 1.1 (MS-DOS 5 and later) | Tested (both listings). |
| GW-BASIC 3.23 | Tested (both listings). |
| BASICA | **Not tested.** It is the same dialect lineage as GW-BASIC, so BASICA from PC DOS 3.x or later is expected to work. Earlier BASICA versions may differ; if one stops with `Out of string space`, use `TRINKSML.BAS`. |
| ROM BASIC (no disk BASIC) | Not possible (see below). |

The BASIC side needs only plain DOS file access and port I/O, so DOS 3.0 or
later should be enough for the loader. That is an expectation, not a test; the
tests ran under an emulated DOS of roughly 5.0 level. QBasic itself implies
DOS 5 or later. This says nothing about what `TRINKET.SYS` itself requires.

## Validation status

The loaders were run in real QBasic 1.0, QBasic 1.1 and GW-BASIC 3.23 under
DOSBox. Only the `INP`/`OUT` lines were replaced, with a software model of the
Trinket's wire protocol and SD command framing written from `wire.c`,
`subsys_sd.c` and `sdtest.c`. Three FAT16 card images were used:

1. MBR + partition type 0Eh, 2 KB clusters, a file whose cluster chain is out
   of order and crosses a FAT-sector boundary, with the `TRINKET.SYS` entry in
   the second root-directory sector among decoy entries.
2. Superfloppy (no MBR), 1-sector clusters, 20,000-byte payload.
3. MBR + partition type 06h, 16 KB clusters.

In every case the copied file matched the source byte for byte (allowing for
the padding described above).

What has **not** been verified:

- **No real hardware.** The model was built from the firmware source, not
  measured on a board. The firmware documentation still marks its SD subsystem
  as unconfirmed against an actual card, and nothing here changes that. Test
  first with a disposable writable disk.
- **Timing.** Handshake waits are loop counts, so their real duration varies
  a lot between CPUs and interpreters. Waits are `TM=100000` iterations, and
  `1000000` while the SD card initializes. If a fast machine reports
  `Parallel-port handshake timed out`, raise `TM` on line 30. A missing cable
  can take a long time to time out on a slow machine; Ctrl-Break stops it.
- **A real low-string-space BASIC.** `TRINKSML.BAS` was run on the interpreters
  above, which do not let a 200-byte string space be enforced. Its low string
  use is by construction (no string longer than one character at a time), not
  by test.
- **BASICA and a real DOS 3.x machine** (see Compatibility).

The loader relies on direct `INP`/`OUT` access to the parallel port. It cannot
run under a protected-mode environment that denies port access (for example a
Windows NT DOS box). Do not run it while another program is using the Trinket
port (including Disney Sound Source software).

## Issues found and fixed during testing

The first draft would not have worked on any of the named interpreters. Real
interpreters and a protocol model found:

- The sector-read routine took the two nibbles in the wrong order, so the very
  first sector read failed.
- A cached FAT sector was reused after the buffer had been overwritten with file
  data, which silently produced a wrong file for any multi-cluster chain.
- `PUT #2,,LEFT$(...)` is a syntax error in QBasic (a variable is required).
- `256*ASC(...)` overflows in QBasic's integer arithmetic for 16-bit fields with
  a high byte of 128 or more.
- GW-BASIC/BASICA: `OPEN ... FOR BINARY` does not exist, strings are limited to
  255 characters, and the variable names `REMAIN` and `TOT` were read as the
  keywords `REM` and `TO`.
- QBasic requires `DIM` of an array before a `DEF FN` that mentions it.

## Requested methods and roadblocks

| Method | Status | Roadblock |
|---|---|---|
| QBasic / GW-BASIC / BASICA | Candidate: [`TRINKET.BAS`](./TRINKET.BAS), fallback [`TRINKSML.BAS`](./TRINKSML.BAS) | Needs a DOS BASIC with `INP`, `OUT`, random-access file I/O and DOS file access. Tested as above; hardware transfer is not. |
| DEBUG | Not supplied | A DEBUG-only loader would need the same port-handshake code, SD command framing, FAT16 root/FAT traversal, and DOS file creation in hand-entered 8086 machine code. A reliable arbitrary-fragmentation version is not a short DEBUG script and is unlikely to meet the 20-30 minute entry target. |
| Built-in XT ROM BASIC | Not supplied | ROM BASIC has no DOS filesystem or DOS `OPEN` support. Its `SAVE` command saves a BASIC program, not a binary DOS driver. Writing a usable `.SYS` file would require machine code/BIOS disk routines, defeating the short BASIC entry goal. |

## Remaining roadblocks

1. **Hardware validation is the main blocker.** The firmware exposes the
   SD-sector request used here, but the firmware documentation marks that
   subsystem unconfirmed against a physical card. The BASIC transfer has no
   hardware test result either.
2. **The typing-time goal is tight for arbitrary FAT16.** The short source must
   do both the parallel-port byte/nibble semaphores and the FAT16 file lookup
   and cluster-chain traversal. This is substantially more code than DEBUG's
   usual short bootstrap snippets.
3. **Target-machine variation matters.** Port base, BASIC dialect/version,
   writable target drive, LPT hardware/status wiring, and available DOS memory
   can differ. The samples default to LPT1 at 378h and drive A:.
4. **No built-in-ROM-only route is available.** Without a disk-capable BASIC
   or already-available DOS tool/driver, stock ROM BASIC cannot create the
   destination file as a DOS driver.

No firmware or driver changes are proposed here. If the SD subsystem does not
work on a particular board/firmware build, solving that would require leaving
the current scope rather than silently adding a new transfer path.
