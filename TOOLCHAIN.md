# Toolchains

This project spans three genuinely different compilers, because its three
build targets run on three different eras of hardware: a 1990s-era 16-bit
DOS driver, a 2020s-era ARM microcontroller, and 16-bit DOS command-line
tools. There's no single toolchain that covers all of it. This document
is a single, honest reference for what each one needs, how reliably each
one can actually be stood up in a fresh sandbox (verified hands-on, not
assumed), and what a future port away from any of them would involve.

Quick status, as of this writing:

| Target                        | Toolchain              | Fresh-sandbox reproducible? |
|--------------------------------|-------------------------|------------------------------|
| DOS driver (TRINKET.SYS)      | Borland C++ 3.1 + dosbox | **No** -- see below |
| RP2040 firmware               | arm-none-eabi-gcc + pico-sdk | Yes, apt + git clone |
| DOS client tools (sdtest.c etc)| ia16-elf-gcc            | **No** -- no viable path found |
| *(not used yet, evaluated)*   | Open Watcom             | Yes, with one documented workaround |

## DOS driver: Borland C++ 3.1 (`legacy/bc31/`) + dosbox

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
```

`build-driver.sh` copies `legacy/bc31/` and `src/*.C`/`*.H`/`*.ASM`/
`MAKEFILE` into a scratch directory, mounts that as dosbox's C:/D:
drives, runs `make -f MAKEFILE` headlessly (`SDL_VIDEODRIVER=dummy`,
`-exit`), and copies the resulting `TRINKET.SYS` out. No X server, no
interactive dosbox window, no dosemu2 -- dosbox alone has been sufficient
for every build this project has needed, compile-side, all project long.

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

`build-firmware.sh` also builds the client tools (`firmware/client/`,
ia16-elf-gcc) as its last step -- see the next section for why that part
is much less reliable.

## DOS client tools (`firmware/client/*.c`): ia16-elf-gcc

**No reliable fresh-sandbox path currently exists.** Checked twice this
project, from scratch:

- Not packaged in apt (`ia16-elf-gcc` isn't a real Debian/Ubuntu package).
- The maintainer's real prebuilt binaries are distributed via a Launchpad
  PPA and GitLab releases -- both outside what a typical sandboxed agent
  environment can reach (network allowlists tend to cover GitHub, PyPI,
  npm, crates.io; Launchpad and GitLab usually aren't on that list).
- Building the full cross-toolchain from source (`build-ia16`'s own
  documented pipeline: binutils -> gcc stage 1 -> newlib -> gcc stage 2,
  against a ~2017-era GCC 6.3) was judged too large and fragile an
  undertaking to attempt for test-client code specifically, versus the
  actual DOS driver.

**Fallback used throughout this project**: `gcc -Wall -Wextra
-fsyntax-only` against the file, treating it as ordinary portable C. This
catches real syntax and type errors and has caught several actual bugs,
but it provably cannot catch everything -- two real, hardware-confirmed
bugs slipped past it and only showed up under a real `ia16-elf-gcc`
build: a 32KB stack buffer that silently violates the 16-bit tiny memory
model's object-size limits (invisible on a 64-bit host, where 32KB is a
trivial stack allocation), and an inline-asm constraint (`"Nd"` for
IN/OUT port operands) that this specific compiler mishandles once the
containing function gets inlined under `-O2` -- fine as a real,
un-inlined function body (which is all it ever was at `-O0`), broken the
moment it's duplicated across many inlined call sites. Both are now
documented in the code comments at the exact lines they bit.

**The practical implication**: any client-tool change made without a
real `ia16-elf-gcc` available should be treated as "syntax-checked, not
verified" until it's actually built and run. That's not a hypothetical
caveat -- it's happened twice.

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
neither Borland (gitignored, no legitimate redistribution path) nor
ia16-elf-gcc (no reachable binary distribution at all) currently is. It
is **not** currently used anywhere in this project -- no driver code has
been ported to it or tested against it, and see the next section for why
that's a real project, not a quick swap.

## Should the driver be ported off Borland?

The honest, evidence-based version of this question, given everything
above: switching the *driver* to ia16-elf-gcc would not help -- it would
extend that toolchain's "syntax-checked, never fully verified" status to
the driver too, which is strictly worse than where the driver stands
today (full real compile+link+disassembly verification, every time, via
dosbox). That's a real downgrade, not a lateral move.

Open Watcom is the toolchain actually worth considering for a future
port, specifically because of what's in the table above: it's the only
16-bit DOS-capable compiler here with both a real automatable fetch path
*and* active maintenance, unlike a 1992 compiler nobody can legally
re-fetch. But porting is genuine work, not a config change:

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

Two real, hardware-confirmed landmines specific to compiling this driver
with Borland C++ 3.1, neither of which a clean compile+link will catch:

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
