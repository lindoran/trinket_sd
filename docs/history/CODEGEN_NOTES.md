# Notes for future Claude (or whoever/whatever else edits this repo)

This project is written with long, deliberately explanatory block
comments -- see any header comment in `src/` for the
house style. That style has one sharp edge worth knowing about before
you add more of it, plus a couple of process notes on how this session
got any confidence at all in code nobody could build.

## The recurring `*/`-inside-a-comment bug

C block comments end at the first `*/` they contain, full stop -- there
is no escaping it. This repo's comment style leans on shorthand like
`DISPATCH_CMD_*` or `CMD_*` a lot, and it is very easy to write two of
those shorthands next to each other separated by a bare slash:

```c
/*
 * ...distinct from both DISPATCH_CMD_* (dispatch.h) and device.h's
 * DOS-side CMD_*/ERR_* -- three different protocols...
 */
```

`CMD_*/ERR_*` contains the substring `*/`. The comment ends right
there, `ERR_*` and everything after it becomes live code, and the
compiler error you get points at wherever the *next* accidental `*/`
or end-of-file is -- not at the actual mistake. This happened twice in
one session while writing `subsys_spi.h` (`CMD_*/ERR_*`) and
`sdtest.c` (`SPI_OP_*/SPI_STATUS_*`), both caught only because this
session happened to compile-check those files (see below) -- it would
NOT have been caught by reading the rendered comment text, since most
editors/reviewers skim past comment prose without noticing the token
boundary.

**Rule going forward: never write two `X_*`-style shorthands separated
directly by a slash.** Put a space in first: `CMD_* / ERR_*`. Same
applies to any other place a literal `*/` could appear unintentionally
inside a comment (a pointer-dereference example, shell glob patterns,
math using `*` and `/` adjacent to each other, etc.) -- if you're about
to type a `*` immediately followed by a `/` anywhere inside a `/* */`
comment, stop and rephrase.

This doesn't apply to `//` line comments (nothing to close early) or to
doc comments in languages that don't use `/* */` at all.

## How this session checked code it couldn't build

There is no `arm-none-eabi-gcc` / vendored `pico-sdk` (for `src/`) and
no `ia16-elf-gcc` (for `driver/` and `client/`) in the sandbox this was
written in -- see `README.md`/`driver/NOTES.md` for the project's own
long-standing "written by inspection, confirm on real hardware" caveat,
which predates this session and isn't specific to it.

What this session *could* do, and did, for every new file:

1. **Host `gcc` with hand-written stub headers.** For `src/subsys_spi.c`,
   wrote minimal stub `hardware/spi.h` / `hardware/gpio.h` /
   `pico/stdlib.h` headers declaring just the functions/types actually
   used (`spi_init`, `spi_set_format`, `gpio_put`, ...), then compiled
   the real source against them with plain host `gcc -c`. This proves
   syntax, types, and control flow are self-consistent -- it proves
   nothing about real RP2040 behavior, real SDK header contents beyond
   what was stubbed, or timing.
2. **Host `gcc` directly, no stubs, for portable logic.** `client/sdtest.c`'s
   port-I/O primitives use inline `outb`/`inb` asm that also assembles
   fine on x86-64 (same mnemonics), so the whole file -- including the
   FAT16 parsing and SD command-sequencing logic, which is ordinary
   portable C with no ia16-specific dependency -- compiled cleanly with
   `gcc -Wall -Wextra` directly. Proves the same things (1) does, for a
   larger fraction of that file, since less of it needed stubbing.
3. **Manual comparison against already-hardware-tested code.** New
   wire-level framing (the dispatch-byte injection in `sdtest.c`'s
   `dispatch_send_spi_write()`/`spi_get_result()`) was checked by
   direct side-by-side comparison against `backend.c`'s
   `backend_write_byte()`/`backend_read_byte()`, which the project
   already treats as hardware-proven. Matching shape isn't a
   substitute for testing, but it's a real reduction in risk over
   writing the framing from scratch.

None of this replaces an actual build + an actual logic analyzer + an
actual SD card. It's the ceiling of what's checkable without them, and
it's what every "UNTESTED / UNBUILT" note elsewhere in this repo from
this session is actually claiming -- not "probably fine," specifically
"passed these three checks and no more."

## Confirmed on real hardware: the "forgot to include the opcode" bug

First real-hardware run of `sdtest.c` (2GB SD card, FAT16) failed at
`sd_init()`'s very first step with `CMD0 failed, r1=0xFF` and a device-
side serial log full of `DISPATCH: unknown command 0x90 -- ignoring`,
`WRITE: received 0x02`, and eventually hundreds of repeats of
`DISPATCH: expected command byte (Write), host selected Read --
ignoring`. Traced byte-for-byte against the log, the actual bug was:
`spi_op_config()` and `spi_op_cs()` built a payload array of just their
*parameters* (`mode, khz_lo, khz_hi` / `level`) and handed it to a
helper documented as "dispatch byte, then the caller-supplied
opcode+payload bytes" -- neither function actually put `SPI_OP_CONFIG`/
`SPI_OP_CS` into that array. `spi_xfer()`, written separately by hand,
happened to get it right. The device read the first *parameter* byte as
the opcode, failed to match it, and the whole byte stream desynced from
there -- every "expected Write, got Read" and "unknown command" line in
the log is a downstream symptom of that one omission, not a separate
problem each.

This is the same root shape as the `CMD_*/ERR_*` comment bug above: a
helper that trusted the caller to remember to include something (there,
a space before a slash; here, an opcode byte) instead of making that
thing impossible to omit. The fix wasn't just "add the two missing
bytes" -- it was changing `dispatch_send_spi_write(bytes, n)` into
`dispatch_send_spi_op(opcode, payload1, n1, payload2, n2)`, an explicit,
required `opcode` parameter, so a future third `spi_op_*` function
*cannot* make the same mistake, the same way the `*/` fix isn't "don't
do that" but "put a space in." When a bug's real cause is "a convention
the caller had to remember," prefer a signature that makes the mistake
impossible over a fix that just corrects the one instance found -- there
are usually more instances waiting.

Practical lesson for reviewing code written by inspection (this file's
own prior "checked by inspection" claim did NOT catch this -- host-gcc
compiling cleanly proves the C is well-formed, not that it sends the
bytes it means to): when a wire-protocol client has several near-
identical "build a request, call a shared sender" functions, check that
EVERY one of them actually includes every fixed field the protocol
requires, byte for byte against the receiving side's parser -- don't
just check that the shared sender's own logic is correct in isolation.

## Open (unconfirmed) investigation: phantom bytes after some XFER reads

A second real-hardware run, after the opcode fix above, showed
`SPI_OP_CONFIG`/`SPI_OP_CS`/each `SPI_OP_XFER`'s Write side all framing
correctly -- but the *read-result* fetch that follows some `SPI_OP_XFER`
calls (`spi_get_result()` in `sdtest.c`) was sometimes trailed by a
consistent ~130 device-side "expected command byte, got Read" lines
before the next real transfer, instead of a clean read. The count
staying roughly fixed rather than growing or shrinking points at
`spi_get_result()` computing a wrong `len` from the `status`/`len_lo`/
`len_hi` bytes it reads back, then trying to read that many more
phantom bytes than the device actually sent (which, per the device's
own printed line, was correct). Current best guess: the Write→Read
direction flip immediately before that first nibble read doesn't have
enough settle margin through the level shifter -- the same class of
issue `ifacetest.c`'s own comments already document for the Strobe
line, just not previously given to the Select line at this specific,
high-frequency transition point.

## RESOLVED: the settle-time hypothesis above was wrong

The investigation above turned out to have the right symptom but the
wrong cause. The `-v` trace it added did its job: it showed the host
reading back `status=0x88 len_lo=0x8E len_hi=0x88` for a transfer the
firmware's own serial log said was `status=0 len=6`. XORing every one
of those observed bytes with `0x88` gives exactly `0x00 0x06 0x00` --
the correct values, byte for byte. That's not a timing glitch (which
would look like occasional, inconsistent corruption); it's a missing,
deterministic transform.

The real cause: `subsys_testharness_run()` (device-side, already
hardware-proven) applies `corrected = last_byte ^ 0x88; // doc sec 7`
to every byte before sending it over the nibble-read mechanism.
`subsys_spi.c`'s equivalent sender never did this -- when it was
written, this session assumed (see `subsys_spi.h`'s original comment,
now corrected) that the XOR was specific to the test harness's own
echo-validation semantics, not something every Read-direction sender
needs. The `-v` trace proves that assumption was wrong: `doc sec 7` is
a wire-protocol-level requirement for the nibble-read mechanism itself
-- every subsystem sending bytes device→host needs it, not just the
one subsystem that happened to be written first and got it right by
following the original protocol doc.

Fixed in `subsys_spi.c`'s `send_next_byte()` (the shared low-level
sender both response bytes go through), not in `sdtest.c` -- the
client's raw reconstruction was always correct, matching
`ifacetest.c`/`backend.c`'s existing "device corrects, host just
reads" pattern. This means **the earlier "just rebuild sdtest.c, no
firmware change needed" guidance for this symptom was wrong** -- this
particular fix needs a firmware rebuild and reflash, unlike the opcode
bug above which really was client-only. `sdtest.c`'s `read_settle_delay()`
was removed (it wasn't wrong to try, but it wasn't the fix, and leaving
speculative dead code with an incorrect rationale next to it would
misdirect the next person); the `-v` trace flag stayed, since it's what
actually solved this and is worth having for the next unexplained
symptom.

Lesson for next time: when a "doc sec N" reference only exists as a
comment citation (the actual protocol doc, `docs/trinket_lpt_protocol.md`,
was referenced by `README.md` but is not present in this archive), don't
assume a correction's *scope* from which subsystem happens to apply it
first -- a citation to a numbered doc section is a strong signal it's a
protocol-level rule, not a per-subsystem choice, even without the doc
itself in hand to confirm.

**Confirmed end-to-end on real hardware** after the fix was actually
reflashed: `SDTEST init` brings up a real SDv2 card, `SDTEST dir` lists
its actual FAT16 root directory correctly, `SDTEST cat` reproduces two
different files' contents exactly. Worth noting explicitly: the retest
right after this fix landed *initially still showed the identical
cascade symptom*, which briefly looked like the fix hadn't worked --
the actual cause was that only `sdtest.c` had been rebuilt, not the
Pico firmware reflashed with the new `subsys_spi.c`. Rebuilding a
firmware source file and flashing the result onto the device are two
separate manual steps here, and it's easy to do the first and skip the
second without any error to flag it -- worth double-checking explicitly
any time a firmware-side fix doesn't produce the expected before/after
change on a retest.

## Namespace hand-sync reminder

Several files define the *same* small integer constants twice on
purpose, because the two sides are built by different toolchains with
no shared header (`src/dispatch.h`'s `DISPATCH_CMD_*` vs
`driver/backend.c`'s copy vs `client/ifacetest.c`'s copy;
`src/subsys_spi.h`'s `SPI_OP_*`/`SPI_STATUS_*`/`SPI_MAX_XFER` vs
`client/sdtest.c`'s copy). Each copy says so in a comment at the
definition site. If you change one side's values, grep for the other
copies before you're done -- there's no compiler to catch a mismatch
here, just a protocol that silently desyncs.
