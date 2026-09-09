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
| *(evaluated, not adopted)*      | dev86/bcc ("Bruce's C Compiler") | Yes, plain `apt-get install` |

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

## Evaluated but not adopted: dev86/bcc ("Bruce's C Compiler")

**Naming collision, worth flagging once clearly**: this section's `bcc`
is dev86's "Bruce's C Compiler," a completely different, unrelated tool
that happens to share Borland's own command name. Every other `bcc` in
this document (the `-Ic:\bc31\include` invocations, `bcc -S`, etc.) means
Borland's compiler. Don't let a script or a memory of this document
conflate the two -- if BC31 and dev86/bcc were ever installed side by
side in the same shell, whichever comes first on `PATH` wins silently.

Investigated 2026-09-08 as a possible answer to a stricter question than
Watcom's: is there a 16-bit DOS toolchain that's *both* trivially
fetchable *and* genuinely free/redistributable (unlike Watcom, whose
binaries are fetchable but whose long-term maintenance is thin; unlike
Borland, which is neither). Verified hands-on -- installed for real,
compiled and ran real code under dosbox, not evaluated from documentation
alone.

**Genuinely attractive, confirmed real:**
- `apt-get install bcc bin86` on stock Ubuntu -- ~342KB download, ~1.2MB
  installed. No PPA, no GitLab, no installer workaround. The easiest
  fetch of any 16-bit DOS-capable compiler this project has evaluated.
- GPL-licensed -- unlike `legacy/bc31/`, this could be vendored directly
  into the repo with no redistribution question at all.
- It does produce working DOS executables: a real compile+link+run of a
  small-model `.COM` file, confirmed executing correctly under dosbox
  (`bcc -Md -ansi -o out.com file.c`, `-Md` being the undocumented-until-
  tested-here MSDOS memory-model preset -- the compiler's own error
  message for an invalid `-M` argument is what revealed the valid letters
  at all, since installed docs were incomplete on this system).
- `int86()` works once hand-declared (present in `libdos.a`, just missing
  from the shipped `bios.h`). Port I/O works via `#asm`/`#endasm` inline
  assembly, once the calling convention is right.

**Real problems found by actually running it, not by reading about it:**
- **ANSI-style typed function prototypes -- what every function in this
  entire codebase uses -- fail to parse at all without `-ansi`** (which
  runs a separate `unprotoize` pass first). Without that flag, `static
  int foo(int a, int b);` is a hard parse error. Matches the compiler's
  own "definitely NOT a true ansi-C compiler" self-description.
- **`printf("%02X", ...)` silently prints lowercase**, not uppercase --
  every hex dump in this project (readsector's dump, FAT16 directory
  listings) would come out lowercase with no error or warning to flag it.
- **The inline-asm calling convention cost a real debugging cycle to get
  right**: writing a port-I/O helper with its own `push bp`/`mov bp,sp`
  frame (the natural way to write it, and what a Borland/Watcom port
  would look like) compiles clean but **hangs with zero output** at
  runtime -- the compiler already emits its own prologue before splicing
  in `#asm` text, so the extra frame nests wrong and corrupts the return
  address. Removing the redundant frame fixes it. Nothing catches this
  at compile time; it has to be run (and crash) to find.
- **No evident device-driver support at all.** dev86's own upstream
  README describes its DOS target explicitly: *"The MSDOS version
  creates small model (64k+64k) **COM files**."* No mention anywhere of
  `.SYS` drivers, no `STRATEGY`/`INTERRUPT` entry-point convention, no
  equivalent to what `HEADER.ASM` + `tlink -t` currently does. `TRINKET.SYS`
  does not appear buildable with this toolchain without inventing that
  machinery from scratch in raw `as86` assembly -- a materially bigger,
  more speculative undertaking than anything else in this document.
- **A completely separate DWORD-arithmetic runtime-helper story** would
  need independent investigation (`libdos.a` has its own `__lsrl.o`/
  `__lmull.o`-style helpers, unrelated to Borland's `N_LXLSH@` family --
  the verification done for BC31 tells us nothing about bcc's version).
- **Thin, hard-to-find documentation in practice.** The apt package didn't
  install working man pages on this system; the actual usable option
  reference came from the compiler's own error messages and reading its
  GitHub source, not from consulting docs the way a Borland or Watcom
  question would resolve in seconds via decades of existing
  forums/manuals.

**Verdict**: real and interesting for the *client tools specifically*
(plain `.EXE`/`.COM` programs, no driver-header machinery needed) if
Borland's redistribution restriction ever becomes a real practical
problem -- the core mechanics are now confirmed to work, and the
remaining gaps (`-ansi` everywhere, the `%X` case bug, redoing the
DWORD-shift check for bcc's own helpers) are known and scoped, not open
questions. Not remotely close to viable for the driver as things stand.
Not adopted anywhere in this project.

## Toolchain philosophy: one canonical toolchain, escape routes elsewhere

Every compiler this project has looked at gets judged against the same
ten requirements. Writing them out explicitly, once, here -- so "why not
Watcom" and "why not bcc" have a fixed answer to point to instead of
being re-argued from scratch each time someone reasonably asks:

1. **Real-mode 16-bit 8086 target** -- not a DOS extender, not 32-bit
   protected mode. This machine's parallel port and BIOS calls are real
   mode; a compiler that can't emit real-mode code natively is out
   regardless of anything else.
2. **Produces `.COM` and `.EXE`** -- the diagnostic clients' format.
3. **Can produce a genuine `.SYS` device driver** -- `TRINKET.SYS`
   specifically needs a `STRATEGY`/`INTERRUPT` far-call entry point and
   the freestanding (non-PSP) loading DOS device drivers use. This is
   the requirement that does the most filtering.
4. **Usable for low-level port I/O and inline assembly** -- the parallel
   port is bit-banged by hand; there's no getting around needing either
   builtin `outportb`/`inportb`-style calls or workable inline asm.
5. **A stable, predictable, documented ABI** -- calling convention, stack
   frame layout, and runtime-helper behavior (32-bit shift/multiply
   especially, see the driver-hazards section below) need to be
   knowable, not reverse-engineered by crashing things.
6. **Works under dosbox** -- this project's entire build pipeline is
   headless dosbox; a toolchain that needs real hardware or a different
   emulator to build under adds a second thing to get working.
7. **Buildable/runnable from a Linux host** -- every build in this
   project runs from a Linux sandbox or CI-equivalent, cross-compiling
   or emulating out to the DOS target.
8. **Small enough to vendor** -- ideally small enough to commit directly
   into the repo as a kit, or at minimum cheap and fast to fetch fresh
   every time.
9. **Open-source and legitimately redistributable** -- no gitignore-and-
   hope-the-user-has-a-copy workaround needed.
10. **Obtainable without a from-source build bootstrap**, and **mature
    enough not to be its own research project** -- a compiler this
    project would need to debug and patch before it could debug and
    patch `TRINKET.SYS` isn't actually saving any work.

|  # | Requirement | BC31 | Open Watcom | dev86/bcc | ia16-elf-gcc |
|---|---|---|---|---|---|
| 1 | Real-mode 8086 | Yes | Yes | Yes | Yes |
| 2 | `.COM`/`.EXE` | Yes | Yes | Yes (COM only) | `.EXE` only |
| 3 | `.SYS` driver | **Yes (proven)** | Unverified | **No** (COM-only target, upstream) | Never attempted |
| 4 | Port I/O / asm | Yes, builtin | Yes, builtin | Yes, but undocumented prologue gotcha (found the hard way) | Inline asm, one confirmed `-O2` constraint bug |
| 5 | Stable/documented ABI | Yes, decades of docs | Yes, decades of docs | **No** -- thin docs, reverse-engineered here via `strings`/crashes | Modern but young for this target |
| 6 | Works under dosbox | **Yes (proven)** | Yes | Yes | N/A (never got this far) |
| 7 | Linux-host buildable | Yes | Yes | Yes | No viable fetch path at all |
| 8 | Small enough to vendor | ~34MB, works | Full install is large ("huge" per the concern that started this) | **~1.2MB, smallest evaluated** | N/A |
| 9 | Open/redistributable | **No** -- proprietary, gitignored | Yes, OSI-approved | **Yes**, GPL | Yes, GPL |
| 10 | No bootstrap, mature | Yes, 1992-vintage, huge legacy userbase | Mostly -- "last official release 1.9... seems to have stalled" per FreeDOS's own devel page | Mixed -- mature codebase, immature-in-practice docs/tooling | Would need building from source, non-trivial |

Reading the table is the honest answer to "why not X":

- **Why not Watcom**: fails #8 outright (the concern that started this
  conversation -- the full distribution is large next to BC31's ~34MB or
  bcc's ~1.2MB) and is soft on #5/#10 (momentum has stalled, and this
  project has never independently verified whether Watcom's runtime has
  its own version of the DWORD-shift-from-a-private-stack hazard found in
  Borland's `cs.lib`). It's the strongest *escape route* specifically
  because it clears #3, #7, and #9 better than the alternatives -- not
  because it's ready to be canonical today.
- **Why not bcc**: fails #3 outright (upstream's own README describes its
  DOS target as small-model COM files, nothing resembling a device-driver
  entry point) and #5 in practice (this session had to find its calling
  convention by crashing a test program, not by reading documentation).
  Wins #8 and #9 cleanly -- smallest and most legitimately open option
  evaluated -- which is exactly why it stays on the board as a *client-
  tools-only* escape route rather than being dismissed outright.
- **BC31 wins on the one requirement that actually matters most**: #3,
  and it's not close -- it's the only toolchain in this table that has
  concretely, on real hardware, built the actual `.SYS` driver this
  project exists around. Everything else being uncertain or imperfect is
  a real cost, but a toolchain that can't clear #3 at all isn't a
  candidate for canonical status regardless of how well it scores
  elsewhere.

**No candidate satisfies all ten.** That's not a failure to search hard
enough -- it's a real, current gap in the 16-bit DOS toolchain landscape,
worth having on record as exactly that rather than re-litigating from
zero the next time a new candidate name comes up.

Given that, the honest shape of this project's toolchain isn't "pick the
best compiler" -- it's one canonical, proven toolchain plus separately-
tracked escape routes, not dependencies:

```
                 TRINKET project
                       |
                       v
              +-----------------+
              |  Canonical DOS  |
              |    toolchain    |
              +-----------------+
              | Borland C 3.1   |
              | TLINK           |
              | TASM            |
              | DOSBox          |
              +--------+--------+
                       |
                  TRINKET.SYS
                  client .EXE
                  client .COM

          Alternative / preservation builds

        dev86/bcc --------- client programs only, unverified DWORD story
        OpenWatcom -------- future investigation, verified fetchable
        IA16 -------------- retired, kept for historical reference
```

BC31 is the toolchain not because it's philosophically ideal but because
it has already, concretely, built the thing this project exists to
preserve -- driver and clients both, hardware-confirmed for the driver,
compile+link+emulated-execution-confirmed for the clients. The other
three rows aren't fallback dependencies this project needs to keep
working; they're documented escape routes for a future where BC31
becomes genuinely unusable. What actually needs preserving is the
*source* -- it's portable knowledge (the wire protocol, the SD/FAT16
logic, the driver's own STRATEGY/INTERRUPT contract) independent of which
compiler happens to eat it. A known-good BC31 binary reference, a known-
good set of outputs, and three independently-evaluated alternative
compilers (one retired, two live options with their tradeoffs written
down) is a stronger preservation story than chasing a single "perfect"
compiler that, per the table above, does not currently exist.

### On requirement #9 and Borland specifically

Worth being direct about, rather than leaving implicit: BC31 fails
requirement 9 for a real reason, not a technicality. Borland C++ 3.1 is
not open-source, and there is no official, legitimate channel to
redistribute or freely obtain it today -- the rights have passed through
several corporate owners since 1992 and nobody currently offers it as
freeware. This is exactly why `legacy/bc31/*` is gitignored and this
project has never bundled it: the repository itself makes no
redistribution claim it can't back up.

In practice, copies of BC31 circulate through the same channels most
abandoned-but-still-useful DOS-era software does -- archive/preservation
sites, hobbyist and retrocomputing communities, and so on. This project
takes no official position beyond what's already implied by the
gitignore: it doesn't host, link to, or provide a copy, and whoever
builds this supplies their own. Where that copy comes from, and how
someone weighs the gap between "abandoned software with no active
rights-holder distributing it" and "technically still under copyright,"
is a judgment call this document isn't going to make on anyone's behalf
-- reasonable, informed people land in different places on that, and the
ambiguity is acknowledged here rather than glossed over.

## If a future Watcom port is undertaken (the concrete work, not just the decision)

Nothing here is currently blocking on a port. BC31 gives every DOS-side
file in this project full real compile+link verification via dosbox,
every time -- strictly better than ia16-elf-gcc's "syntax-checked, never
fully verified" status ever was for the clients, and there's no analogous
gap for the driver to begin with. The only real pressure toward a future
port is Borland's own redistribution restriction (`legacy/bc31/*`
gitignored, supplied out-of-band by whoever's building this) -- not a
technical shortcoming of BC31 itself.

Open Watcom is the toolchain actually worth reaching for if that pressure
ever does force the question (see "Toolchain philosophy" above for why,
against dev86/bcc, ia16-elf-gcc, and the full requirements checklist).
But porting is genuine work, not a config change, and now applies to two
source trees (`src/`, `firmware/client/`) instead of one:

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

Worth doing eventually, given the redistribution problem Borland has --
but scope it as its own project, not a quick swap alongside other driver
work.

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
