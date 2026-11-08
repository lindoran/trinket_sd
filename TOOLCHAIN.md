# Build environment

This project builds with two genuinely different compilers because it runs
on two eras of hardware: 16-bit DOS code (the driver and the test tools) and
a 2020s ARM microcontroller (the firmware).

| Target | Output | Toolchain | Reproducible from a plain clone? |
|---|---|---|---|
| RP2040 firmware (`firmware/src/`) | `trinket_sd.uf2` | `arm-none-eabi-gcc` + Pico SDK 2.3.0 | Yes: `apt` + `git clone` |
| DOS driver (`src/`) | `TRINKET.SYS` | Borland C++ 3.1 + TASM, run under DOSBox | **No**: you supply Borland C++ 3.1 |
| DOS tools (`firmware/client/`) | `SDTEST.EXE`, `IFTEST.EXE`, `DACTEST.EXE`, `DSSTEST.EXE`, `DSSSTRES.EXE`, `DSSCFG.EXE` | Borland C++ 3.1, run under DOSBox | **No**, same reason |

All of the above was built from this tree in a clean Ubuntu 24.04 sandbox
(with a supplied Borland copy): `./build.sh` produced the firmware, the
driver and all six tools with no compiler errors, and the host-side unit
tests in `firmware/tests/` pass.

## Quick setup

```
apt-get install gcc-arm-none-eabi libnewlib-arm-none-eabi cmake git dosbox
apt-get install p7zip-full zip        # only for `make package`

./check-toolchain.sh                  # reports what is present and missing
./build.sh                            # everything -> dist/
./clean-all.sh
```

`make` wraps the same scripts (`make help` lists the targets). Individual
scripts: `build-firmware.sh`, `build-driver.sh`, `build-client.sh`, each with
a matching `clean-*.sh`. `build.sh` runs all three even if one fails, prints
an OK/FAILED summary, and exits non-zero if anything failed.

`make package` writes the firmware/driver/tools release archives and a
`trinket_sd-source.tar.gz` source archive to `packages/`. That directory is
separate from `dist/` and is not removed by `make clean`.

## RP2040 firmware

`setup-firmware.sh` (run automatically by `build-firmware.sh`) clones the
Pico SDK at the pinned tag (`2.3.0`, with submodules) into
`~/.pico-sdk/sdk/2.3.0` if it is not already there, and links the system
`arm-none-eabi-gcc` into the layout the SDK expects. Set `PICO_SDK_PATH` to
use an existing SDK checkout instead.

**`picotool` is optional.** It is not an apt package on a stock Ubuntu, and the
Pico SDK fetches and builds it from source the first time CMake configures
(you will see `No installed picotool with version 2.3.0 found - building from
source`; that is normal, and it makes the first firmware build slower).

The result is `firmware/build/trinket_sd.uf2`, copied to `dist/`.

## DOS driver and tools: Borland C++ 3.1 + DOSBox

**You must supply Borland C++ 3.1 yourself.** It is proprietary software this
project has no right to redistribute, so `legacy/bc31/*` is gitignored (only a
`.gitkeep` ships). A plain `git clone` or "Download ZIP" will never contain a
compiler there. `build-driver.sh` and `build-client.sh` stop with a message
saying so if it is missing. This is expected, not a broken checkout.

Needed at minimum under `legacy/bc31/`: `BIN/BCC.EXE`, `BIN/MAKE.EXE`,
`BIN/TLINK.EXE`, `BIN/TASM.EXE`, plus `INCLUDE/` and `LIB/`.

On where to get it: Borland C++ 3.1 has passed through several corporate
owners since 1992 and nobody currently distributes it officially. Copies
circulate through archive and retrocomputing channels. This project does not
host, link to, or provide a copy, and takes no position on the
abandonware-versus-still-copyrighted question. Whoever builds this makes
that call.

How the build runs: each script copies `legacy/bc31/` and the sources into a
scratch directory, mounts it as DOSBox's `C:`/`D:`, runs `make -f MAKEFILE`
headlessly (`SDL_VIDEODRIVER=dummy`, `-exit`), and copies the products into
`dist/`. No X server and no interactive window are involved. The generated
files never land in the checkout.

On a real DOS machine, set up Borland C++ 3.1 the usual way, then `cd src` and
run `make -f MAKEFILE`:

```
SET INCLUDE=C:\BC31\INCLUDE\
SET LIB=C:\BC31\LIB
SET TEMP=C:\TEMP
PATH C:\;C:\BC31\BIN;%PATH%
```

### Build pitfalls worth knowing before touching a Makefile or adding a file

1. **`bcc -o` names the object file, not the `.EXE`.** `bcc -ms
   -oSDTEST.EXE sdtest.c` compiles cleanly, then fails at the automatic link
   step with `Unable to open file 'sdtest.obj'`. For a one-step build, do not
   pass `-o`: `bcc` names the `.EXE` after the source file. (`src/MAKEFILE`
   avoids this by compiling with `bcc -c` and calling `tlink` itself.)
2. **DOS 8.3 file names are enforced for real under DOSBox.** A source file
   called `ifacetest.c` was invisible to the compiler even though it was in the
   mounted directory, which is why the file is `iftest.c`. Any new DOS-side
   source file needs an 8.3-safe name from the start.
3. **C89 only.** `for (int i = ...)` loop-scoped declarations and
   declarations after statements are rejected (`Expression syntax`,
   `Undefined symbol 'i'`, `Statement missing ;`). Declare everything at the
   top of its block. `//` line comments are accepted by `bcc` and are used
   throughout.
4. **Check how a build script detects failure.** `bcc`'s errors look like
   `Error dactest.c 4: message`, while TASM prints `Error messages:    None`
   on every clean build. `build-driver.sh` and `build-client.sh` therefore
   match lines starting with `Error ` and then discard the `Error messages:`
   summary line, and they check that every expected output file exists. When
   adding a tool, add it to the script's existence check, the copy step and
   `clean-client.sh`, or a failed build can be reported as success.

## Known driver-specific hazards

Two hardware-confirmed landmines affect compiling **the driver** (`src/`)
with Borland C++ 3.1. A clean compile and link does not catch either. Both
come from things only the driver does: being invoked on DOS's
`STRATEGY`/`INTERRUPT` private-stack calling convention, and receiving far
pointers to buffers DOS placed without this project's control. Neither is
expected to apply to the plain `.EXE` tools, which have a normal C startup
and own their buffers statically.

1. **A 32-bit (`DWORD`) shift or multiply** anywhere reachable from
   `STRATEGY`/`INTERRUPT` (almost everything in `TRINKET.C`, `DRIVER.C` and
   `SD.C`) compiles to a call into Borland's `cs.lib` helpers
   (`N_LXLSH@`/`N_LXURSH@`). Called from the driver's small private stack they
   crash with exception 06 (the exact mechanism was never pinned down).
   Fix pattern used throughout: do the arithmetic in 16-bit (`WORD`) pieces
   (see `advance_far_ptr()`, the byte-reinterpretation in `sd_arg_for_lba`-style
   code, and `bios_ticks()`'s direct-memory-write construction of a `DWORD`
   from two `WORD` halves). **Verify** any new 32-bit arithmetic in this call
   path with `bcc -c -ms -Z -O -Ol -Oe -S file.c` and `grep N_LX` on the
   resulting `.ASM`. Do not trust that it "looks like" the safe pattern.
   `MK_FP`/`FP_SEG`/`FP_OFF` are safe (a Borland intrinsic, not long
   arithmetic).

2. **Plain `far` pointer arithmetic only touches the 16-bit offset** and
   never carries into the segment. `p += 512`, `p++` and **array indexing**
   (`buf[i]`) all behave identically (confirmed in the disassembly: `add
   bx,si` with `ES` never adjusted). A transfer into a buffer within the last
   few hundred bytes of its 64 KB segment silently wraps and corrupts memory.
   This is reachable on an ordinary single-sector transfer, since it depends
   only on where DOS placed the caller's buffer. Fix: walk a local far pointer
   with `advance_far_ptr(p, N)` (per byte, `advance_far_ptr(p, 1)`, inside a
   tight loop) instead of `+=`, `++` or indexing. A per-byte loop that indexes
   `buf[i]` internally reintroduces the bug even when the outer per-sector loop
   is correct.

Other facts found the hard way: `cdprintf()` (`CPRINT.C`) supports only
`%c`, `%d`, `%x` (one-digit width), `%L` (`DWORD` hex) and `%s`, not `%u`, and
an unsupported specifier silently misaligns every later argument. And the
tools: `sdtest.c` and `dactest.c` contain `N_LX` call sites that were checked
(`bcc -S`, plus an execution test of the same shift/multiply patterns under
DOSBox) and found to be in ordinary `push bp`/`mov bp,sp` frames. That covers
what existed when it was checked; run the same `-S`/`grep N_LX` check on new
32-bit arithmetic in any tool. `dsstest.c`, `dssstres.c` and `dsscfg.c`
also call the `N_LX` helpers (15, 15 and 2 call sites) in ordinary
foreground code. `DSSSTRES` is the one tool that installs an interrupt
handler, so its `isr8` was checked specifically: its generated assembly
contains no `N_LX` call (every call site is in foreground code). Static check
only, not run on hardware.

## Contributor notes

- **Protocol constants are duplicated on purpose.** The firmware
  (`firmware/src/dispatch.h`, `subsys_*.h`) and the DOS side (`src/TRINKET.C`
  and the clients in `firmware/client/`) are built by different toolchains
  with no shared header. Each copy says so at its definition. If you change a
  `DISPATCH_CMD_*`, `SD_OP_*`, `SPI_OP_*` or `DAC_OP_*` value, grep for every
  copy. There is no compiler to catch a mismatch, only a protocol that
  silently desyncs.
- **Never write `*/` inside a `/* ... */` comment.** The project uses long
  explanatory block comments, and shorthand like `CMD_*/ERR_*` closes the
  comment early and turns the rest into broken code, with an error pointing
  somewhere else entirely. Write `CMD_* / ERR_*`.
- **Reflashing is a separate step from rebuilding.** A firmware fix does
  nothing until the new UF2 is flashed. Several apparent "the fix didn't work"
  results during development were an un-reflashed Pico.
- **Firmware changes can be compile-checked here; DOS changes can be built but
  only proven on hardware.** DOSBox verifies that the DOS side compiles and
  links, and is good for pure logic, but timing-sensitive parallel-port
  behaviour needs the real thing.

## Why Borland, and the alternatives that were evaluated

Borland C++ 3.1 is the DOS toolchain because it is the only candidate that has
actually built the `.SYS` driver this project exists around, on real hardware.
It fails one requirement, open redistribution, and no evaluated candidate
meets all of them (Open Watcom, dev86/bcc and ia16-elf-gcc each fall short
somewhere else, most importantly on producing a genuine `.SYS` driver or on
being buildable at all). The full requirements table and the hands-on
evaluation of each alternative are kept in
[docs/history/TOOLCHAIN-evaluation.md](./docs/history/TOOLCHAIN-evaluation.md).
