# Toolchains

This project spans two genuinely different compilers, because its build
targets run on two different eras of hardware: 1990s-era 16-bit DOS code
(the driver AND the diagnostic clients) and a 2020s-era ARM
microcontroller. This document is a single, honest reference for what
each one needs, how reliably each one can actually be stood up in a
fresh sandbox (verified hands-on, not assumed), and what a future port
away from either would involve.

**Toolchain decision (2026-09-08): Borland C++ 3.1 is this project's one
DOS-side toolchain, driver and diagnostic clients alike.** It used to be
two -- BC31 for the driver, ia16-elf-gcc for `firmware/client/*.c` --
because the driver needs a real freestanding `.SYS` binary and the
client tools started out as ia16-elf-gcc experiments before BC31 was
vendored. ia16-elf-gcc never got a working fresh-sandbox install path
(see the retired section this replaced, preserved in git history) and
left every client-tool change "syntax-checked, never fully verified"
until it happened to reach real hardware. BC31 has neither problem: it's
already vendored, already proven end-to-end (TRINKET.SYS, real hardware,
the Wolf3D milestone), and now builds `sdtest.c`/`iftest.c` too --
confirmed via a real compile+link under the vendored `bcc`/`tlink`
(headless dosbox), zero errors, zero warnings, on the first attempt once
two DOS-toolchain-specific build gotchas (below) were worked out. See
`build-client.sh`.

Quick status, as of this writing:

| Target                          | Toolchain                    | Fresh-sandbox reproducible? |
|----------------------------------|-------------------------------|------------------------------|
| DOS driver (TRINKET.SYS)        | Borland C++ 3.1 + dosbox      | **No** -- see below |
| DOS diagnostic clients (SDTEST.EXE, IFTEST.EXE) | Borland C++ 3.1 + dosbox | **No** -- same reason as the driver |
| RP2040 firmware                 | arm-none-eabi-gcc + pico-sdk  | Yes, apt + git clone |
| *(retired 2026-09-08)*          | ia16-elf-gcc                  | No -- see git history for the old writeup |
| *(evaluated, not adopted)*      | Open Watcom                   | Yes, with one documented workaround |

## DOS driver + diagnostic clients: Borland C++ 3.1 (`legacy/bc31/`) + dosbox

**The binaries are deliberately not in git.** `legacy/bc31/*` is gitignored
(only a `.gitkeep` ships) because Borland C++ 3.1 is old proprietary
software this project has no right to redistribute. This means: a plain
`git clone` or a GitHub "Download ZIP" of this repo will **never** include
a working compiler here, and `build-driver.sh` will fail with a clear
error explaining exactly this if it's missing. This isn't a broken
checkout -- it's expected, and it was genuinely surprising to rediscover
mid-session after an earlier sandbox happened to inherit a real copy from
a raw filesystem tarball rather than a git export. If you're setting this
up fresh, you need to supply your own legally-obtained copy of Borland
C++ 3.1 at `legacy/bc31/` yourself (`BIN/`, `INCLUDE/`, `LIB/` at minimum
-- `BIN/BCC.EXE`, `BIN/MAKE.EXE`, `BIN/TLINK.EXE`, `BIN/TASM.EXE` are what
`build-driver.sh` and `src/MAKEFILE` actually invoke).

Once that's in place:

```
apt-get install dosbox   # or your distro's equivalent
./build-driver.sh        # -> dist/TRINKET.SYS
./clean-driver.sh
./build-client.sh        # -> dist/SDTEST.EXE, dist/IFTEST.EXE
./clean-client.sh
```

`build-driver.sh` copies `legacy/bc31/` and `src/*.C`/`*.H`/`*.ASM`/
`MAKEFILE` into a scratch directory, mounts that as dosbox's C:/D:
drives, runs `make -f MAKEFILE` headlessly (`SDL_VIDEODRIVER=dummy`,
`-exit`), and copies the resulting `TRINKET.SYS` out. `build-client.sh`
is the same shape one level down (`firmware/client/*.c` +
`firmware/client/MAKEFILE`), producing `SDTEST.EXE`/`IFTEST.EXE`. No X
server, no interactive dosbox window, no dosemu2 -- dosbox alone has been
sufficient for every build this project has needed, compile-side, all
project long.

**Two build-system gotchas found while porting the clients to BC31,
neither driver-specific -- worth knowing before touching either
Makefile or adding a new DOS-side source file:**

1. **`bcc`'s `-o` flag names the *object* file, not the final `.EXE`.**
   `bcc -ms -oSDTEST.EXE sdtest.c` compiles clean with zero diagnostics,
   then fails at the auto-invoked link step with `Fatal: Unable to open
   file 'sdtest.obj'` -- the compiler wrote its object out as
   `SDTEST.EXE` (per `-o`) instead of the `SDTEST.OBJ` the linker still
   goes looking for by default. Fix: don't pass `-o` for a single-file
   one-step build at all; bcc names the `.EXE` from the source basename
   automatically, which is already what's wanted. (The driver's own
   `src/MAKEFILE` never hit this because it uses `bcc -c` to compile
   only, then invokes `tlink` directly by hand with explicit names --
   the `-o`-names-the-object behavior is only a trap for a combined
   compile+link invocation.)
2. **DOS 8.3 filenames are a real, enforced constraint under dosbox +
   this compiler**, not just a cosmetic old-DOS quirk. `ifacetest.c` (9
   characters before the extension) doesn't fit and produced `Error:
   Could not find file 'ifacetest.c'` even though the file was right
   there in the mounted directory -- dosbox silently exposes it under a
   truncated/generated alias instead. Renamed to `iftest.c` (6
   characters) to fix this permanently rather than work around it per
   build; this also finally makes the source filename match the
   `IFTEST.EXE` name its own Makefile target and usage docs already
   used, which never actually agreed with `ifacetest.c`/`IFACETEST.EXE`
   even before this port. Any future DOS-side source file (the dropped
   `dactest.c` included, if/when that work resumes) needs an 8.3-safe
   name from the start.

**Why not dosemu2 too?** It was mentioned early on as a possible
fallback, but it's never actually been used or needed -- every single
successful build this project has done used dosbox, and dosemu2 isn't
apt-installable in the sandbox this was written in either, so a "detect
and fall back to dosemu2" branch would be shipping untested code for a
tool this project has zero track record with. `build-driver.sh` is
structured as a single clearly-separated dosbox path specifically so a
dosemu2 branch could be added later by someone who can actually verify
it, rather than guessing at commands now.

**Verifying beyond "it compiled":** a clean compile+link doesn't catch
this project's two real, previously-hard-won hazards -- see the
"Known driver-specific hazards" section below. Checking for those needs
an extra step (`bcc -S` and grepping the output), not just running the
Makefile.

## RP2040 firmware: arm-none-eabi-gcc + pico-sdk

This one reproduces cleanly from a fresh sandbox:

```
apt-get install gcc-arm-none-eabi cmake git
./build-firmware.sh      # -> firmware/build/trinket_sd.uf2
./clean-firmware.sh
```

`setup-firmware.sh` (called automatically by `build-firmware.sh`) clones
pico-sdk at the exact pinned tag (`2.3.0`, with submodules) from GitHub
if it isn't already at `~/.pico-sdk/sdk/2.3.0`, and symlinks the system
`arm-none-eabi-gcc`/`picotool` into the layout it expects.

**One real gap found while writing this**: `setup-firmware.sh` also wants
`picotool` on PATH, and `picotool` isn't apt-installable in at least one
sandbox tested. It turns out not to matter -- it's only used by the setup
script's own idempotency check, not by the actual build. If you hit
`picotool is missing`, a plain `cmake -S firmware -B firmware/build
-DPICO_SDK_PATH=... -DPICO_BOARD=pico && cmake --build firmware/build`
produces a complete, valid `.uf2` with no picotool involved at all.

`build-firmware.sh` no longer builds the DOS client tools as a last step
(that moved to `build-client.sh`/BC31, 2026-09-08 -- see the next
section).

## Retired: DOS client tools on ia16-elf-gcc

`firmware/client/*.c` (`sdtest.c`, the file now named `iftest.c`) used to
target ia16-elf-gcc. That toolchain never got a working fresh-sandbox
install path in two separate attempts -- not packaged in apt, real
binaries only via a Launchpad PPA and GitLab releases (both outside
typical sandbox network allowlists), and building the full
binutils/gcc/newlib cross-toolchain from source was judged too large an
undertaking to justify for test-client code specifically. The fallback
(`gcc -fsyntax-only` against the file as ordinary portable C) caught real
bugs but provably not everything: a 32KB stack buffer that silently
violated the 16-bit tiny model's object-size limits, and an inline-asm
operand constraint (`"Nd"`) this compiler mishandled once inlined under
`-O2` both slipped past it and only surfaced under a real build. Every
change made without real ia16-elf-gcc access carried a standing
"syntax-checked, not verified" caveat as a result.

As of 2026-09-08 these files are ported to Borland C++ 3.1 instead (see
the toolchain-decision note and the two build gotchas at the top of this
document) -- a real compile+link is available in every sandbox that has
`legacy/bc31/` supplied, the same as the driver, so this caveat no longer
applies to them. The old GCC inline-asm port-I/O (`outportb`/`inportb`)
and the raw `int $0x1a` timing call are both gone, replaced by BC31's
`<dos.h>` builtins and `int86()` respectively -- see `iftest.c`/
`sdtest.c`'s own header comments for exactly what changed and why the
rest of the logic (handshake sequencing, SD command layer, FAT16
parsing) carried over unmodified.

## Evaluated but not adopted: Open Watcom

Investigated as a possible answer to "is there a 16-bit DOS toolchain
that's actually easy to stand up in a sandbox" -- verified hands-on, not
from documentation alone:

- **Real GitHub Release binaries exist** at
  `github.com/open-watcom/open-watcom-v2/releases`, tag `Current-build`
  (also monthly-dated tags). Fetchable with a plain `wget`/`curl` from a
  domain most sandboxes can already reach -- no PPA, no GitLab, no
  license friction (Sybase Open Watcom Public License, OSI-approved).
- **The official self-extracting installer crashes immediately**
  (`Floating point exception`, `SIGFPE`) in at least one real sandbox
  tested, regardless of flags or a faked terminal size. Root cause not
  chased down further than "this particular installer stub doesn't like
  this particular container."
- **Workaround, verified working**: the "self-extracting" file is a real
  ZIP with an installer stub prepended, and 7-Zip reads it directly
  (`Warning: The archive is open with offset` -- harmless): `7z x
  -o<destdir> open-watcom-2_0-c-linux-x64` extracts the full ~4000-file
  payload without ever running the crashing stub. From there, `wcc`
  (compiler) and `wlink` (linker) work as plain command-line tools with
  `WATCOM`/`PATH`/`INCLUDE` set conventionally.
- **Confirmed end-to-end**: compiled a small-memory-model 16-bit C file
  and linked it into a real DOS MZ executable (`file` confirms `MS-DOS
  executable, MZ for MS-DOS`) using nothing but this extracted tree.

This is real, working evidence that Open Watcom is available in a way
Borland (gitignored, no legitimate redistribution path) currently isn't.
It is **not** currently used anywhere in this project -- no driver or
client code has been ported to it or tested against it, and see the next
section for why that's a real project, not a quick swap. The
ia16-elf-gcc option this paragraph used to also compare against is
retired project-wide as of 2026-09-08 (see above) -- Watcom's comparison
point now is Borland alone, not a three-way choice.

## Should this project be ported off Borland?

This used to be asked driver-only; as of 2026-09-08 (BC31 now covers the
clients too -- see the toolchain-decision note at the top of this
document) it's really one question for the whole DOS side. The honest,
evidence-based version, given everything above: nothing here is
currently blocking on it. BC31 gives every DOS-side file in this project
full real compile+link verification via dosbox, every time -- strictly
better than ia16-elf-gcc's "syntax-checked, never fully verified" status
ever was for the clients, and there's no analogous gap for the driver to
begin with. The only real pressure toward a future port is Borland's own
redistribution restriction (`legacy/bc31/*` gitignored, supplied
out-of-band by whoever's building this) -- not a technical shortcoming
of BC31 itself.

Open Watcom is the toolchain actually worth considering if that pressure
ever does force the question, specifically because of what's in the
table above: it's the only 16-bit DOS-capable compiler here with both a
real automatable fetch path *and* active maintenance, unlike a 1992
compiler nobody can legally re-fetch. But porting is genuine work, not a
config change, and now applies to two source trees (`src/`,
`firmware/client/`) instead of one:

- **The DWORD-shift/cs.lib hazard (see below) is Borland-specific and
  unverified for Watcom.** Whether Watcom's runtime has an analogous
  "some 32-bit operations are unsafe in this driver's private-stack call
  path" landmine is completely unknown -- that fact took real hardware
  debugging (an exception 06 crash) to discover for Borland, and would
  need to be independently rediscovered or ruled out for Watcom before
  trusting any of TRINKET.C's arithmetic under it.
- **`HEADER.ASM` is hand-written TASM-syntax assembly.** Watcom ships its
  own assembler (WASM) with its own syntax dialect -- this needs porting
  or continuing to shell out to TASM as an external dependency either
  way.
- **Everything needs re-verification on real hardware**, not just a
  clean compile -- codegen differences between compilers can change
  timing-sensitive behavior (the parallel-port bit-banging in Section 1)
  in ways a build system can't catch.

Worth doing eventually, given the redistribution problem Borland has that
this session only just surfaced -- but scope it as its own project, not a
quick swap alongside other driver work.

## Known driver-specific hazards (Borland C++ 3.1, this call path)

Two real, hardware-confirmed landmines specific to compiling **the
driver** (`src/`) with Borland C++ 3.1, neither of which a clean
compile+link will catch. Both are tied to things only the driver does --
being invoked on DOS's `STRATEGY`/`INTERRUPT` private-stack calling
convention, and receiving far pointers to buffers DOS itself placed
without this project's control. **Neither is expected to apply to
`firmware/client/*.c`**: those are plain `.EXE`s with a normal C startup
(never on a driver's private stack) and own all their buffers statically
(never a DOS-supplied far pointer crossing a segment boundary this
project doesn't control) -- see `iftest.c`/`sdtest.c`'s own header
comments for that reasoning spelled out per-file.

**Checked, not just reasoned through (2026-09-08):** `bcc -S` on
`sdtest.c` confirms `N_LXLSH@`/`N_LXURSH@`/`N_LXMUL@` genuinely get
called (from `sd2_read_sectors`/`sd2_write_sectors`/`sd_cmd`/
`sd_arg_for_lba`/`rd_u32`/`fat16_mount`/`fat16_cluster_lba`/
`fat16_next_cluster`/`cmd_bench`/`cmd_fastbench` -- `iftest.c` has none
at all, no DWORD arithmetic there) -- and every call site sits inside a
completely ordinary `push bp`/`mov bp,sp` frame with BP-relative locals,
nothing resembling a private stack. Beyond the static check, a
standalone harness reproducing the exact same shift/multiply/div
patterns, built the same `-ms` way, was actually **run** under dosbox
(not just compiled) and returned correct results for all of them, no
crash -- about as close to a real hardware proof as this sandbox can
get. Residual gap: that's dosbox's 8086-compatible CPU emulation, not a
period-real chip, though this class of operation (integer
shift/multiply/call/ret, nothing timing- or peripheral-sensitive) is
exactly where emulator fidelity is least likely to diverge. Same
`bcc -S`/`grep N_LX` recipe (below) still worth running on any *new*
client-side 32-bit arithmetic before trusting it -- this check covers
what exists in `sdtest.c`/`iftest.c` today, not future changes.

1. **A 32-bit (`DWORD`) shift or multiply**, anywhere in the code path
   reachable from `STRATEGY`/`INTERRUPT` (i.e. almost everything in
   `TRINKET.C`/`DRIVER.C`/`SD.C`), compiles to a call into Borland's
   `cs.lib` runtime helpers (`N_LXLSH@`/`N_LXURSH@`). Calling those from
   this driver's small private stack crashes with exception 06 (exact
   mechanism never fully pinned down). Fix pattern used throughout: do
   the arithmetic in 16-bit (`WORD`) pieces instead (see
   `advance_far_ptr()`, `sd_arg_for_lba`-style byte-reinterpretation, and
   `bios_ticks()`'s direct-memory-write construction of a `DWORD` from
   two `WORD` halves). *Verify* any new 32-bit arithmetic in this call
   path with `bcc -c -ms -Z -O -Ol -Oe -S file.c` and `grep N_LX` on the
   resulting `.ASM` -- don't just trust that it "looks like" the safe
   pattern.

2. **Plain `far` pointer arithmetic only touches the 16-bit offset**,
   never carrying into the segment -- `p += 512`, `p++`, and **array
   indexing** (`buf[i]`) all have this problem identically (confirmed via
   disassembly: `add bx,si` with `ES` never adjusted). A transfer into a
   buffer positioned within the last few hundred bytes of its 64K segment
   silently wraps and corrupts memory instead of crossing into the next
   segment -- reachable on an ordinary single-sector transfer, not just a
   multi-sector edge case, since it depends only on where DOS happened to
   place the caller's buffer. Fix: walk a local far pointer forward with
   `advance_far_ptr(p, N)` (or per-byte, `advance_far_ptr(p, 1)` inside a
   tight loop) instead of `+=`/`++`/indexing directly.
