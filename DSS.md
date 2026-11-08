# Disney Sound Source emulation

The trinket can act as a Disney Sound Source (DSS) on its own parallel port.
A game that already knows how to drive a DSS writes the port directly. The
RP2040 sees those writes on the same wires the dispatch protocol uses, plays
the audio through the PCM5102 path, and answers the game's "FIFO full" poll.
No host software, no TSR, no V86 trapping, so it works on an 8088 with
unmodified games.

This file is the reference for how that works and what has and has not been
confirmed. For setup and everyday use see [README.md](./README.md). The
development notes that led here (several rounds of change, with their
then-current "not yet run on hardware" caveats) are kept in
[docs/history/DSS-development-notes.md](./docs/history/DSS-development-notes.md).

## What is confirmed, and what is not

All hardware results are from one bench rig (VIA Eden thin client, real
parallel port, FreeDOS). Nothing here has run on a real 4.77 MHz XT.

**Confirmed on hardware**

- `DSSTEST detect`, `tone` and `play` work. Stock Wolfenstein 3D plays
  digitized sound through the trinket.
- The interrupt guard in `TRINKET.SYS` (`/l=`) works. `DSSSTRES` passes with
  the guard on, fails with `/l=0` (the collision being reproduced), and
  `/INJECT` is caught. Roughly 97 % of the stress tool's timer interrupts
  are lost to the interrupts-off windows during nonstop SD reads, and the
  DOS clock slips accordingly (about 358 s in one run, corrected from the RTC).
- Strobe capture is armed from the nAuto-LF edge interrupt. Measured entry
  latency is about 1 us, and every feeder strobe is captured.
- nAck is raised one byte early and the strobe loop runs from RAM. In
  Wolfenstein 3D runs, strobes dropped as FIFO overflow fell from about
  7.1 % to about 0.05-0.07 %, with no wire timeouts or phantom resets.
- With these in place the 358-session Wolf3D run (`/l=1`) showed zero DAC
  underruns. Residual audible problem: mild dropouts only in the demo, which
  loads data from disk while samples play.

**Not confirmed**

- The deeper FIFO (`/f=`, `/w=`, `DSSCFG`, section 6). Implemented, and its
  byte protocol, FIFO model, warm-up policy and power tracker are unit-tested
  on a host, but no hardware run is on record.
- Behaviour on a real XT-class machine: strobe low-time, ISR interaction, and
  the code actually executing on an 8088.
- Strobe edge quality on pin 17 through the MOSFET level shifter at 1-10 us
  pulses (a scope trace would settle it).
- Any game other than Wolfenstein 3D. Other games may detect or drive the
  unit differently.

## 1. What a DSS-aware program does

From the DSS Programmer's Guide:

| Signal | LPT pin | Role |
|---|---|---|
| D0-D7 | 2-9 | sample byte |
| nSelect-In | 17 | strobe: pulse LOW for about 1-10 us. The **rising edge** clocks the byte into the unit's 16-deep FIFO. Held HIGH = unit powered |
| nAck | 10 | status bit 6: **HIGH = FIFO full** |
| nInit | 16 | only supplies current to the unit's status transistor, held HIGH |

The unit drains its FIFO at a fixed ~7 kHz (+/- 5 %). Reference control
values: `04h` power-up / strobe released, `0Ch` strobe low / power-down.
Games detect a DSS by writing more than 16 bytes and looking for nAck.
Wolfenstein 3D's `ID_SD.C` strobes with `ssOn = 04h` / `ssOff = 0Ch` and
tests status bit 6 (`0x40`), exactly the sequence this firmware keys on.

## 2. How the firmware tells "a game" from "the dispatcher"

`04h` and `0Ch` both have control bit 1 clear (**pin 14 HIGH**) and bit 2
set (**pin 16 HIGH**). The trinket's own protocol never has both high:

| Host state | control | pin 14 (nAuto-LF) | pin 16 (nInit) |
|---|---|---|---|
| dispatcher idle | `02h` | low | low |
| dispatcher transaction | `02h` + Init pulses | low | pulses |
| host reset | `00h` | **high** | low |
| **DSS session** | `04h` / `0Ch` | **high** | **high** |

`wire_dss_signature()` (one atomic `gpio_get_all()` read) is that test.
While it is true, `main.c` runs `subsys_dss_run_session()` instead of the
dispatcher, and when it drops the dispatcher resumes. The reset interrupt
also fires on every DSS power-up (pin 14 rises), so `main.c` checks the
signature first, waits 5 us for the level-shifter channels to settle, and
`wire_do_reset_sequence()` returns `false` if it sees nInit rise while "in
reset" (that was a DSS power-up, nothing to reset).

## 3. Data path

```
 game OUT ──► D0-D7 ─┐
 game strobe ► pin17 ┴► dss_capture.pio (pio1): on rising edge, sample D0-D7
                              │ RX FIFO (8 deep)
                              ▼
                  subsys_dss.c session loop (core 0)
                     ├─► dss_model.c: 16-deep FIFO, 7 kHz drain clock
                     │        full flag ──► GPIO19 / LPT pin 10 (nAck)
                     └─► one sample per drain tick (popped byte, or the
                         held last value when empty)
                              ▼
              subsys_dac_dss_push(): same ring buffer, same DMA/I2S/PCM5102
```

The audio never crosses the dispatch wire, so SD traffic does not share
bandwidth with it. The only coupling is the arbitration issue in section 5.

Pin check against the board: D0-D7 are inputs through the LVC245; the strobe
(pin 17) comes through the MOSFET level shifter; nAck (pin 10) is GPIO19 on
the AHCT245, already a nibble output. Nothing needs open-collector drive.

Implementation details that matter when changing the firmware:

- **Session entry.** The nAuto-LF edge has its own RAM-resident interrupt
  handler at the top NVIC priority. When nInit is also high it arms the PIO
  capture immediately, so the state machine's 8-deep RX FIFO holds what
  arrives before the session loop starts. This protects the first 8 strobes
  of a burst.
- **nAck one byte early.** nAck goes up when the FIFO holds 15 bytes
  (`DSS_FULL_LEVEL`), one short of the 16-byte capacity. A game that
  re-reads nAck only microseconds after a strobe could otherwise slip one
  extra byte past a flag that was a little late. Cost: one byte (143 us) less
  lead. Wolf3D's presence test (32 blind writes, then expects "full") still
  passes.
- **RAM-resident hot path.** The strobe loop, the FIFO model's write/advance
  and the DAC push run from RAM and make no calls into flash. nAck is driven
  with an inline GPIO write. Drain-tick work runs only when a tick is due.
- **Underruns are prime-aware.** A DSS session only starts counting
  underruns once the ring has held one full 256-frame DMA chunk.
- **After a gap** (the dispatcher had the port) the model plays out what a
  real unit would have played meanwhile, usually the whole FIFO, which
  empties in 2.3 ms, then re-anchors, so a game's next burst refills all slots.

## 4. Testing

Build with `./build-firmware.sh` and `./build-client.sh`, flash the UF2, and
with the trinket on the port you will use (`/P=378`):

1. `DSSTEST detect`. Exit code 0 means pass. Expect: nAck low after
   power-up, high after 32 back-to-back bytes, low again 10 ms later, and 15
   writes (17 at most) to fill the FIFO.
2. `DSSTEST rate 3`. The drain clock as the host sees it, timed on the BIOS
   tick. Expect about 7000 Hz (a few percent of resolution).
3. `DSSTEST tone 440 3`. Listen. Wrong pitch means a wrong drain rate.
4. `DACTEST status`. `underruns` should be 0 or very close to it.
5. Stock Wolfenstein 3D with Disney Sound Source selected on the trinket's port.
6. `DSSSTRES read <file> 20` and `DSSSTRES write TEST.TMP 128`, the
   interrupt-collision stress test (section 5).

`DSSTEST play <file.raw> [loops] [/B=<sectors>]` streams **unsigned 8-bit mono
raw audio at 7000 Hz**. Any other format plays as noise. The file is loaded
into RAM before the first sample and playback never touches the disk,
because a real DSS has a 16-byte FIFO (2.3 ms) and a read through
`TRINKET.SYS` takes far longer than that. A file bigger than free memory is
played in segments with an audible gap per segment. `/B=<sectors>` caps the
segment size in 512-byte sectors.

Firmware diagnostics: **ESC on the firmware's USB console** prints the DSS
counters (strobes captured, FIFO level and max, `overflow(dropped)`, drain
ticks played/held/gap-drained, ring depth, underruns, entry-path and
entry-latency statistics). It is serviced between commands, so after a game
session it answers once the next access to the card happens. ESC also
toggles verbose logging, resets every subsystem and plays a short beep, so
never press it during a test run. `nAuto-LF rising edges` versus `reset
sequences completed` is worth watching: completed resets climbing during a
run that never resets the driver would mean a game's port writes are being
mistaken for a reset. `overflow(dropped)` rising on a fast host would mean
nAck is being raised too late.

## 5. Game interrupts versus the dispatcher on the same wires

A game's timer ISR writes the data and control ports as absolute values at
any instruction boundary, and nothing arbitrates with `TRINKET.SYS`. Two
failure modes follow:

- **Silent corruption (writes).** `raw_send_byte()` is `OUT data` then
  `ctrl_out(INIT_RAISED)`. An ISR between the two leaves the game's sample
  byte on the data pins when Init rises, and the Pico latches that instead of
  the driver's byte. Reads have the same shape through the nibble select.
- **Aborted transaction.** The ISR's `04h`/`0Ch` writes put pins 14 and 16
  high, which is the DSS signature, so the firmware abandons the in-flight
  command and DOS reports an error.

Wolfenstein 3D only exposes this while a digitized sample is playing
(`SDL_SSService()` loops on `while (ssSample)`, at 700 Hz, in bursts of about
10 strobes).

**The guard.** `TRINKET.SYS` runs every data-moving transaction with
interrupts off (`wire_lock()`/`wire_unlock()` in `TRINKET.C`: flags saved with
`pushf`, `cli`, and `sti` only if IF was set on entry, checked in the
compiler's assembly output). `disk_read()`/`disk_write()` split larger
requests into chunks of `/l=<n>` sectors (default 1), with interrupts
re-enabled between chunks so a pending game ISR, the timer and the keyboard
are serviced at each boundary. `/l=0` turns the guard off. `SD_OP_INIT` is not
guarded. A finer-grained alternative (short `cli` windows around only the
unsafe instruction pairs, plus firmware that ignores a DSS signature
mid-transaction) would avoid the sound gap entirely but is much more work and
is not done.

**Cost.** While a chunk runs the Sound Source stream is starved (a real
unit's FIFO is 16 bytes, so any window past about 2 ms is audible), and the
system timer can lose a tick once a window outlasts about 55 ms. One sector
is about 17 ms on the bench rig; a 4.77 MHz XT will be slower. `/l=1` is
already the smallest window.

### `DSSSTRES`

Installs the same kind of timer interrupt Wolf3D uses while a digitized sound
plays (PIT channel 0 at about 700 Hz, chained to the BIOS handler, each tick
running the same service loop as `SDL_SSService()`), while the foreground
reads or writes a file on the trinket drive and checks every byte.

```
DSSSTRES read  <file> [passes]    [/P=378] [/INJECT] [/SLIP=<s>]
DSSSTRES write <file> [kilobytes] [/P=378] [/INJECT] [/SLIP=<s>]
```

- `read`: `<file>` is any existing file on the trinket drive, ideally 256 KB
  or more. A baseline pass with no feeder records its CRC-32, and every
  feeder-on pass (read size cycles 512/2048/8192/16384 bytes) must match it.
- `write`: creates a scratch file on the SD card with the feeder running,
  stops the feeder, reads it back, compares against a pseudo-random pattern,
  and **deletes it**. A collision here silently corrupts a sector, so use a
  scratch card or a throwaway name.
- `/INJECT` corrupts one byte on purpose to prove the checker reports a
  failure and exits non-zero. `/SLIP=<s>` self-tests the clock resync by
  knocking `<s>` seconds off the clock at the end.
- Exit code 0 = clean, 1 = failure detected, 3 = could not run. DOS critical
  errors are counted instead of prompting. The summary reports how far the
  DOS clock slipped (interrupts lost to interrupts-off windows are ticks the
  clock never sees) and, with an RTC, puts the clock back.

With `/l=0` it is meant to be able to fail. If it never fails there, the
window was too narrow to hit on that machine and the run proves nothing
about the guard either way: try more passes, a bigger file or a slower
machine.

## 6. Deeper FIFO (experimental, off by default)

The remaining audible problem is the brief dropout when a game reads from the
SD card while a sound plays: the game's timer interrupt is held off while the
driver has the port, a real unit's 16-byte FIFO (2.3 ms) runs dry, and
nothing can be played that was never written. A deeper FIFO lets the game's
interrupt, on the chance it gets between chunks, write further ahead while the
trinket keeps that audio playing. Depth 16 is the default and behaves exactly
as in section 3.

**Setting it.** In `CONFIG.SYS`: `DEVICE=TRINKET.SYS ... /f=<depth> /w=<ms>`
(re-sent on every card initialisation), or at the DOS prompt:

```
DSSCFG            show the current depth and warm-up
DSSCFG 256        depth 256, warm-up 250 ms
DSSCFG 256 100    depth 256, warm-up 100 ms
DSSCFG default    back to 16 / 250 ms
```

Depth is 16-512 bytes (at 7000 samples/s about 250 covers a 34 ms
transaction, 512 about 73 ms). The setting lives in the trinket's RAM: a
host reset does not clear it, a trinket power cycle does. Wire command
`DISPATCH_CMD_DSS` (05h): op `01h` SET + depth(2) + warm-up(2); the read
direction returns 7 bytes (status, depth, warm-up, max).

**How it works.**

1. *Deep FIFO.* nAck is raised one byte short of the configured depth. A
   program that polls nAck before each write simply writes more per interrupt.
2. *Flush on session end.* When a session ends (the dispatcher takes the
   port) the FIFO contents are handed to the DAC ring, where the I2S DMA plays
   them on its own clock. Room is left in the ring (256 frames); anything that
   does not fit stays queued.
3. *Warm-up, so detection still works.* Programs detect a Sound Source by
   writing more than 16 bytes blind and expecting "full". A program that has
   just powered the unit up gets the real 16-byte FIFO for `warm-up` ms before
   the deep one takes over. "Just powered up" means either that a session
   started after the program had been away from the port for 1 s or more, or
   that pin 17 went high after being low for 1 ms or more. Short absences (a
   disk transaction) are not power-ups.

**Things to watch for:**

- The warm-up rule is an inference from Wolfenstein 3D's detection code and
  the DSS guide. A program that detects mid-run would fail detection at a deep
  depth. Run `DSSCFG default` first.
- Latency rises by up to depth/7000 s plus whatever the flush queues, and a
  program that sequences on "sample finished" will see it finish early by the
  amount buffered.
- `DSSSTRES` caps its feeder at 32 writes per interrupt, so it does not
  exercise a deep FIFO. Use `DSSTEST detect` (it should pass at any depth; run
  it after the port has been idle for a second so it counts as a fresh
  power-up) and a real game.
- The ESC diagnostics show `FIFO depth: configured=... now ... warm-ups=...
  flushed to DAC=...`. `flushed to DAC` climbing during a game's disk loads is
  the feature working.

## 7. Known limits

1. **Disk reads still gap the sound.** With the guard on, a game reading
   from the SD card mid-sound gaps for the length of each chunk, as a real DSS
   would behind a slow disk. A burst-transfer redesign of the wire protocol
   (not started) would shorten each chunk and matters most for XT audio.
2. **Latency about 0.1 s.** Samples go through the existing 256-frame DMA
   chunks (36.6 ms each at 7 kHz, two in flight, plus the ring's own fill).
   Shrinking the chunk size at low rates would cut it; it is shared with the
   44.1 kHz path, so it was left alone.
3. **The Tandy variant (`0Eh` strobe) is not supported.** It sets control
   bit 1, so pin 14 drops on every strobe and the signature flickers.
4. **A few pops** in a tone test with `held(empty)` above 0 in the ESC
   console mean the host stalled for longer than the FIFO's 2.3 ms (BIOS, SMI
   or a timer interrupt) and the model correctly held the last value, as a real
   DSS would.
