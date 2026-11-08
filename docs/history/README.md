# Development history

Working notes, superseded plans and test logs from building the trinket.
They are kept for the record and for the reasoning in them. **They are not a
current reference, and several contradict each other or the code.** For
current information use [../../README.md](../../README.md),
[../../DSS.md](../../DSS.md), [../../TOOLCHAIN.md](../../TOOLCHAIN.md) and
[../../firmware/README.md](../../firmware/README.md).

Each file is the unedited original except where noted. Where two of them
disagree, the code and the current top-level docs win.

| File | What it is | Where it is out of date |
|---|---|---|
| `README-pre-launch.md` | Top-level README before the launch rewrite | "Where this is headed" still describes audio as a future, open design question (it is built and working); says a card swap is "probably fine", whereas the card is fixed media; has no description of the DOS tools and no instructions for setting the trinket up |
| `firmware-README-pre-launch.md` | `firmware/README.md` before the rewrite (its hardware tables were carried forward) | Says 4 subsystems (there are 5, plus Sound Source mode); says `build-firmware.sh` builds the DOS tools (it does not); says host-side media-change caching is a pending follow-up (implemented in `SD.C`); "next validation steps" were all done; treats `picotool` as required (it is optional) |
| `ENHANCEMENTS.md` | The original optimisation proposal (5 points) plus a status section | Problem statements are written in the present tense about code that no longer works that way; estimated ~90 % less handshake traffic, measured ~26 % faster; status table says multi-block reads are "planned" while the text says "on hold" |
| `SOUND.md` | Early PCM5102 bring-up notes and goals | Calls for an `IA16` C program (that toolchain was retired); the multiplexing goal was solved by the `/l=` interrupt guard and the Sound Source mode, not by the approach hinted at here |
| `DSS-development-notes.md` | The Sound Source design doc as it grew over several rounds | Many "not yet run on hardware" caveats were resolved afterwards by real runs (see `logs/`); header says first results are in while section 7 lists them as unconfirmed; claims ESC prints counters mid-session (it only answers between commands); lost-tick cost "not yet measured" (since measured). Superseded by `../../DSS.md` |
| `TOOLCHAIN-evaluation.md` | The full toolchain write-up, including the requirements table and the Open Watcom, dev86/bcc and ia16-elf-gcc evaluations | Says "two gotchas" and lists four; says `build-client.sh` makes only `SDTEST.EXE`/`IFTEST.EXE`; says `picotool` must be present. The evaluation sections remain the only record of why Borland was chosen, and are still valid. Current build instructions are in `../../TOOLCHAIN.md` |
| `CODEGEN_NOTES.md` | Notes written for a future maintainer while debugging early SPI work | Refers to files that no longer exist (`backend.c`, `driver/`, `ifacetest.c`, `docs/trinket_lpt_protocol.md`) and to an `ia16-elf-gcc` toolchain; states no ARM compiler was available (now it is); contains an "open investigation" that its own later section marks RESOLVED. The two lasting lessons (never write `*/` inside a block comment; protocol constants are duplicated by hand) are now in `../../TOOLCHAIN.md` |
| `dsstest-B-sectors.patch` | A patch changing `DSSTEST`'s `/B=` unit from KB to sectors | Already applied to the tree (verified: it reverse-applies cleanly), so it has no further use |
| `logs/testwolf-l1.log`, `logs/testwolf-l2.log` | Serial-console captures from Wolfenstein 3D runs at `/l=1` (358 sessions) and `/l=2` (109 sessions) on the bench rig | These are the evidence for the Sound Source capture and overflow figures quoted in `../../DSS.md`. They predate the deeper-FIFO feature, so they say nothing about it |

## A note on source-code comments

Comments in the firmware and DOS sources refer to some of these files by
their old names and locations: `ENHANCEMENTS.md`, `SOUND.md` and
`docs/CODEGEN_NOTES.md`. Those now live in this directory. The sources were
left untouched.
