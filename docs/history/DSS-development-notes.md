# Disney Sound Source emulation

The trinket can act as a Disney Sound Source (DSS) on its own parallel
port. A game that already knows how to drive a DSS writes the port
directly; the RP2040 sees those writes on the same wires the dispatch
protocol uses, plays the audio through the existing PCM5102 path, and
answers the game's "FIFO full" poll. No host software, no TSR, no V86
trapping -- so it works on an 8088, with unmodified games.

Status: first hardware results are in -- `DSSTEST detect`/`tone`/`play`
work on the board and stock Wolfenstein 3D plays flawlessly through it,
including while the demo loads. Section 7 lists what is still
unconfirmed; section 6 has the one known problem (disk reads during a
foreground stream).

## 1. What a DSS-aware program does (from the DSS Programmer's Guide)

| Signal | LPT pin | Role |
|---|---|---|
| D0-D7 | 2-9 | sample byte |
| nSelect-In | 17 | strobe: pulse LOW ~1-10 us; the **rising edge** clocks the byte into the unit's 16-deep FIFO. Held HIGH = unit powered |
| nAck | 10 | status bit 6: **HIGH = FIFO full** |
| nInit | 16 | only supplies current to the unit's status transistor; held HIGH |

The unit drains its FIFO at a fixed ~7 kHz (+/- 5 %). Reference control
values: `04h` power-up / strobe released, `0Ch` strobe low / power-down.
Games detect a DSS by writing more than 16 bytes and looking for nAck.

## 2. How the firmware tells "a game" from "the dispatcher"

`04h` and `0Ch` both have control bit 1 clear (**pin 14 HIGH**) and
bit 2 set (**pin 16 HIGH**). The trinket's own protocol never has both
high:

| Host state | control | pin 14 (nAuto-LF) | pin 16 (nInit) |
|---|---|---|---|
| dispatcher idle | `02h` | low | low |
| dispatcher transaction | `02h` + Init pulses | low | pulses |
| host reset | `00h` | **high** | low |
| **DSS session** | `04h` / `0Ch` | **high** | **high** |

`wire_dss_signature()` (one atomic `gpio_get_all()` read) is that test.
While it is true, `main.c` runs `subsys_dss_run_session()` instead of
the dispatcher; when it drops, the dispatcher resumes. The reset IRQ
also fires on every DSS power-up (pin 14 rises), so `main.c` checks the
signature **first**, waits 5 us for the two level-shifter channels to
settle, and `wire_do_reset_sequence()` now returns `false` if it sees
nInit rise while "in reset" (that was a DSS power-up, nothing to reset).

## 3. Data path

```
 game OUT ──► D0-D7 ─┐
 game strobe ► pin17 ┴► dss_capture.pio (pio1): on rising edge, sample D0-D7
                              │ RX FIFO (8 deep)
                              ▼
                  subsys_dss.c session loop (core 0, ~0.3 us per pass)
                     ├─► dss_model.c: 16-deep FIFO, 7 kHz drain clock
                     │        full flag ──► GPIO19 / LPT pin 10 (nAck)
                     └─► one sample per drain tick (popped byte, or the
                         held last value when empty)
                              ▼
              subsys_dac_dss_push(): same ring buffer, same DMA/I2S/PCM5102
```

The audio never crosses the dispatch wire, so SD traffic doesn't have
to share bandwidth with it; the only coupling is the arbitration issue
in section 6.

## 4. Pin check against the board

- D0-D7 (pins 2-9): LVC245, host -> Pico. Used as inputs. OK.
- Strobe (pin 17): MOSFET level shifter, host -> Pico. OK.
- nAck (pin 10): AHCT245, Pico -> host. GPIO19, already a nibble output. OK.
- Nothing needs open-collector drive.

## 5. Testing

Build: `./build-firmware.sh` and `./build-client.sh` (adds `DSSTEST.EXE`).
Flash the UF2. With the trinket on the port you will use (`/P=378`):

1. `DSSTEST detect` -- exit code 0 = pass. Expect: nAck low after
   power-up, high after 32 back-to-back bytes, low again 10 ms later,
   15 writes (17 at most) to fill (the trinket raises nAck one byte short of the 16-byte FIFO, see 6.7).
2. `DSSTEST rate 3` -- the drain clock as the host sees it, timed on
   the BIOS tick. Expect ~7000 Hz (a few % of resolution).
3. `DSSTEST tone 440 3` -- listen. Wrong pitch = wrong drain rate.
4. `DACTEST status` -- `underruns` should be 0 (a count of 1 per
   session was normal before the priming fix below; it counted the first
   DMA chunk finding the ring still filling).
5. Stock Wolf3D (unpatched) with DSS selected on the port the trinket is
   plugged into. Compare against the patched `ID_SD.C` build.

6. `DSSSTRES read <file> 20` and `DSSSTRES write TEST.TMP 128` -- the
   interrupt-collision stress test, see section 5a.

While a session is running, **ESC on the firmware's USB console** prints
the DSS counters without disturbing playback (strobes captured, FIFO
level/max, `overflow(dropped)`, drain ticks played/held/gap-drained, ring
depth, underruns). Pressed when the firmware is idle between dispatches,
ESC instead toggles verbose debug logging, dumps everything and resets the
subsystems -- press it again to turn logging back off, and never during a
test run. The link status block also shows `nAuto-LF rising edges` and
`reset sequences completed`: every DSS session entry edges nAuto-LF, so
completed resets climbing during a run that never resets the driver would
mean a game's port writes are being mistaken for a reset. `overflow(dropped)` rising on a fast host would mean nAck
is being raised too late (see 7).

### 5a. Interrupt-collision stress test (`DSSSTRES`)

Proves (or disproves) that the `TRINKET.SYS` interrupt lock works.
`DSSSTRES` installs the same kind of timer interrupt Wolf3D uses while a
digitized sound plays -- PIT channel 0 at ~700 Hz, chained to the BIOS
handler so the DOS clock stays right, each tick running the same Sound
Source service loop as `SDL_SSService()` in `ID_SD.C` (status bit 6
clear -> data byte, strobe `0Ch`, `04h`) -- while the foreground reads or
writes a file on the trinket drive and checks every byte.

```
DSSSTRES read  <file> [passes]   [/P=378] [/INJECT]
DSSSTRES write <file> [kilobytes][/P=378] [/INJECT]
```

- `read`: <file> is any existing file on the trinket drive, ideally
  256 KB or more. A baseline pass with no feeder records its CRC-32;
  every feeder-on pass (read size cycles 512/2048/8192/16384 bytes) must
  match it.
- `write`: creates a scratch file on the SD card with the feeder running,
  stops the feeder, reads it back, compares against the pseudo-random
  pattern, and **deletes it**. This is the direction where a collision
  silently corrupts a sector, so use a scratch card or a throwaway name.
- `/INJECT` corrupts one byte on purpose, to prove the checker reports a
  failure and exits non-zero.
- Exit code 0 = clean, 1 = failure detected, 3 = could not run. DOS
  critical errors are counted instead of prompting Abort/Retry/Ignore.
- It plays a tone throughout, so you can hear the gap each disk chunk
  makes. ESC on the firmware's USB console prints the DSS session
  counters (a session re-entry per interrupt burst is expected).

The summary also reports how far the DOS clock slipped during the run
(its handler passes the BIOS only the interrupts it actually serviced, so
every interrupt lost to an interrupts-off window is a tick the clock never
sees) and, when there is an RTC, puts the clock back on exit. `/SLIP=<s>`
is a self-test of that: it knocks `<s>` seconds off the clock at the end
so you can see the resync report about that figure and fix it.

What to expect: with `TRINKET.SYS` at its default `/l=1` it should PASS
(it also passed on the board at `/l=2` and `/l=3`).
With `/l=0` in CONFIG.SYS (lock off) it is *meant* to be able to fail --
that is the collision being reproduced. If it never fails with `/l=0`,
the window was simply too narrow to hit on that machine and it proves
nothing about the lock either way; try more passes, a bigger file, or a
slower machine. On a machine with an AT-style real-time clock and a run
of 10 s or more it also reports how many timer interrupts the
interrupts-off windows cost.

Verified without hardware: it builds clean under BC31 and, run under
DOSBox with its emulated Disney Sound Source, the feeder strobes, read and
write modes pass, `/INJECT` is reported as a failure with a non-zero exit
code, a missing file exits 3, and the timer and DOS clock are restored on
exit. **Not** verified: any run against the trinket, the DOS critical
error handler, and the lost-tick measurement.

## 6. Known limits and next steps

1. **Game ISR vs. dispatcher on the same wires -- mitigated, untested.**
   A game's timer ISR writes the data and control ports as absolute
   values, at any instruction boundary, and nothing arbitrates with
   TRINKET.SYS. Two failure modes, both read from `TRINKET.C`/`wire.c`:
   - *Silent corruption (writes).* `raw_send_byte()` is `OUT data` then
     `ctrl_out(INIT_RAISED)`. An ISR between the two leaves the game's
     sample byte on the data pins when Init rises, and the Pico latches
     that instead of the driver's byte (command, LBA or sector data).
     Reads have the same shape: the ISR's absolute control write can
     change the nibble select between "select" and "read status".
   - *Aborted transaction.* The ISR's `04h`/`0Ch` writes put pins 14 and
     16 high, which is the DSS signature, so the firmware abandons the
     in-flight command. `DRIVER.C` never retries: the driver returns an
     error, DOS shows Abort/Retry/Ignore, and `InitNeeded` forces a
     re-init (a host reset, which also flushes the DAC ring).
   Wolf3D only exposes this while a digitized sample is playing:
   `SDL_SSService()` loops on `while (ssSample)`, so with no sample
   active the ISR never touches the port (it runs at TickBase*10 = 700 Hz
   during a sample, in bursts of ~10 strobes).

   **The driver now runs every data-moving transaction with interrupts
   off** (`wire_lock()`/`wire_unlock()` in `TRINKET.C`: flags saved with
   `pushf`, `cli`, and `sti` only if IF was set on entry; verified in the
   compiler's assembly output, 8086-clean). `disk_read()`/`disk_write()`
   split larger requests into chunks of `/l=<n>` sectors (default 1), with
   interrupts re-enabled between chunks so a pending game ISR, the timer
   and the keyboard are serviced at each chunk boundary. `/l=0` turns it
   off. `SD_OP_INIT` is not locked (card bring-up can be long and nothing
   is streaming then). The finer-grained alternative -- short `cli`
   windows around the unsafe instruction pairs, plus firmware that
   ignores a DSS signature mid-transaction and keeps capturing strobes
   while it waits on the wire -- would avoid the sound gap entirely but
   is much more work and is not done.

   `DSSSTRES` (section 5a) is the test for this.

   Cost, not yet measured on hardware: while a chunk runs the Sound
   Source stream is starved (a real unit's FIFO is 16 bytes, so any
   window past ~2 ms is audible), and the system timer can lose a tick
   once a window outlasts ~55 ms (one sector is ~17 ms and two ~34 ms at the Eden
   rig's ~17 ms/block; a 4.77 MHz XT will be slower, and `/l=1` is already
   the smallest window, so any clock drift there means the transfers
   themselves are slow).
2. **Disk reads still gap the sound.** Observed on hardware:
   `DSSTEST play` breaks up badly when the audio file lives on the
   trinket SD card and is fine from any other drive. That is expected,
   not a firmware fault: the tool is a foreground loop with no timer
   ISR, a real DSS FIFO is 16 bytes (2.3 ms), and every `fread` through
   TRINKET.SYS both takes tens of milliseconds and takes the port away
   from the Sound Source while it runs. `DSSTEST play` therefore now
   preloads the file into far memory before the first sample and never
   touches the disk during playback (segmented, with an audible gap per
   segment, if the file exceeds free memory; `/B=<sectors>` caps the
   segment size in 512-byte sectors). A game gets away with disk access
   mid-sound because its timer ISR keeps feeding the FIFO -- which is the collision in item 1.
   With the interrupt lock in place the game's ISR is held off while the
   driver owns the wire, so a game reading from the SD card mid-sound
   will gap for the length of each chunk, exactly as a real DSS would
   behind a slow disk. Wire time
   per sector on a 4.77 MHz bus is unmeasured and probably much worse
   than the Eden rig's ~17 ms/block, which is why the paused
   burst-transfer redesign now matters for XT audio.
3. **Latency ~0.1 s.** Samples go through the existing 256-frame DMA
   chunks (36.6 ms each at 7 kHz, two in flight, plus the ring's own
   fill). Shrinking the chunk size at low rates would cut it; left
   alone because the chunk size is shared with the 44.1 kHz path.
4. **Tandy variant (`0Eh` strobe) is not supported.** It sets control
   bit 1, so pin 14 drops on every strobe and the signature flickers.
5. After a gap (the dispatcher had the port) the model plays out what a
   real unit would have played meanwhile -- usually the whole FIFO, which
   empties in 2.3 ms -- and then re-anchors, so the FIFO reads empty on
   re-entry and a game's next burst refills all 16 slots. The held-value
   ticks of the gap are not back-filled (that would just add latency).
   Before this fix the model froze while the session was away, stayed
   "full" across every disk chunk, and the feeder in `DSSSTRES` got only
   ~2 bytes in per timer interrupt (visible as strobes ~= 2 x interrupts
   in its summary). A few strobes at the very start of each re-entry are
   still lost: the PIO capture is armed first thing but the main loop
   needs a few microseconds to notice the signature. Compare the host's
   strobe count with `Strobes captured` in the ESC diagnostics to see how
   many.

   *Session-entry latency (second round).* On the first hardware run only
   ~57% of the feeder's strobes were captured: roughly the first dozen of
   each burst fell in the ~80 us before the main loop started the session.
   Three changes, none yet run on hardware:
   - The nAuto-LF edge now has its own RAM-resident interrupt handler at
     the top NVIC priority (the SDK's callback path lives in flash and can
     stall on a cold cache line, and at the default priority it queues
     behind USB and the audio DMA interrupt). When nInit is also high it
     arms the PIO capture straight away; the state machine's 8-deep RX
     FIFO holds what arrives until the session loop starts. Verified from
     the ELF that both functions sit in RAM and make no calls into flash.
   - The main loop polls the USB console every 20 ms instead of on every
     pass, so it isn't sitting in TinyUSB at the instant a session starts.
   - New instrumentation in the ESC diagnostics: how many sessions were
     armed by the edge interrupt vs. the main loop, how many found the
     PIO FIFO already full (more than 8 strobes arrived before the main
     loop got there, so the state machine stalled and strobes were lost),
     and a histogram of edge-to-session-start time.
   Limit: this protects the first 8 strobes. If the histogram still shows
   many entries at 50 us or more, the FIFO-full flag (nAck) is also late and
   the feeder overshoots; the real fix for that is servicing the capture
   from the second core, which is not done.
6. **Underrun counting is prime-aware.** A DSS session only starts
   counting underruns once the ring has held one full 256-frame DMA
   chunk; before that the first chunk is guaranteed to find the ring
   short (it takes 36.6 ms of audio to fill) and was being counted as an
   underrun at every session start. Session re-entry also no longer
   rewrites the I2S divider or re-initialises the SCLK pin when the rate
   and clock mode are unchanged, and the PIO capture is armed before
   anything else on entry so a re-entry loses less of the first strobe.
   A few pops in a tone test with `held(empty)` > 0 in the ESC console
   mean the host stalled for more than the FIFO's 2.3 ms (BIOS/SMI/
   timer interrupt), and the model correctly held the last value, just
   as a real DSS would.

7. **nAck is raised one byte early, and the strobe loop runs from RAM.**
   The first Wolf3D run with the hardware instrumentation showed 7.1% of the
   game's strobes dropped as FIFO overflow (23,179 of 327,467; the stress
   tool, with a slower C-compiled feeder, dropped 0.02%). Wolf3D's service loop
   (`SDL_SSService`) re-reads nAck only a couple of microseconds after each
   strobe; our flag has to cross the PIO, the session loop and the output
   buffer first and was sometimes that late, so one extra byte slipped in at the
   end of a burst and was discarded. Two changes (not yet run on hardware):
   - nAck now goes up when the FIFO holds 15 bytes (`DSS_FULL_LEVEL`), one
     short of its 16-byte capacity. A flag that is one strobe late then lands
     in the spare slot and is accepted, not dropped. Cost: one byte (143 us)
     less lead. Wolf3D's presence test (`SDL_CheckSS`: 32 blind writes, then
     expects "full") still passes; `DSSTEST detect` now expects 15 writes to
     fill instead of 16.
   - The strobe loop, the FIFO model's write/advance and the DAC push now run
     from RAM and never call into flash (checked in the ELF: the loop's only
     calls are those three RAM functions), nAck is driven with an inline GPIO
     write, and the drain-tick work (64-bit time, model advance, DAC push)
     runs only when a tick is due, found with a single 32-bit compare, instead
     of on every pass. The DMA interrupt is masked in the DAC push through the
     NVIC registers directly (same operations as `irq_set_enabled()`, without
     the flash-resident call).
   How to tell if it worked: `overflow(dropped)` in the ESC diagnostics after
   a Wolf3D run should fall from ~7% of `Strobes captured` to near zero.

## 6a. FIFO depth (new, experimental, off by default)

The remaining audible problem is the brief dropout when a game reads from the
SD card while a sound plays: the game's timer interrupt is held off while the
driver has the port, a real Sound Source's 16-byte FIFO (2.3 ms) runs dry,
and nothing can be played that was never written. A deeper FIFO lets the
game's interrupt -- on the chance it gets between chunks -- write further
ahead, and the trinket keeps that audio playing while the driver has the port.
Normal mode is unchanged: **depth 16 is the default and behaves exactly as
before.**

**Setting it.** In CONFIG.SYS: `DEVICE=TRINKET.SYS ... /f=<depth> /w=<ms>`
(driver re-sends it on every card initialisation), or at the DOS prompt:

```
DSSCFG            show the current depth and warm-up
DSSCFG 256        depth 256, warm-up 250 ms
DSSCFG 256 100    depth 256, warm-up 100 ms
DSSCFG default    back to 16 / 250 ms
```

Depth is 16-512 bytes (at 7000 samples/s: ~250 covers a 34 ms transaction,
512 about 73 ms). The setting lives in the trinket's RAM; a host reset does
not clear it, a trinket power-cycle does (the driver re-sends it at boot).
New wire command `DISPATCH_CMD_DSS` (05h): op `01h` SET + depth(2) +
warm-up(2); the read direction returns 7 bytes (status, depth, warm-up, max).

**How it works.**
1. *Deep FIFO.* nAck is raised one byte short of the configured depth, as for
   16. A program that polls nAck before each write simply writes more per
   interrupt (up to depth - 1 bytes), so the FIFO sits nearly full.
2. *Flush on session end.* The strobe loop only runs while a Sound Source
   session is active, so a deep FIFO's contents would sit there while the
   driver has the port. When a session ends (the dispatcher takes the port)
   the FIFO is handed to the DAC ring, where the I2S DMA plays it on its own
   clock. Room is left in the ring (256 frames); anything that doesn't fit
   stays queued. Over a long load, every chunk boundary gives the game's
   interrupt one chance to refill, and at depth 256 that is ~36 ms of audio
   per chance against ~17 ms of disk time per chunk, so supply can exceed
   consumption and the audio can stay continuous.
3. *Warm-up, so detection still works.* Programs detect a Sound Source by
   writing more than 16 bytes blind and expecting "full" (Wolfenstein 3D's
   `SDL_CheckSS`: 32 writes, then a status check). A deep FIFO would fail
   that. So a program that has just powered the unit up gets the real 16-byte
   FIFO for `warm-up` ms before the deep one takes over. "Just powered up"
   means either that a session started after the program had been away from
   the port for 1 s or more, or that pin 17 went high after being low for
   1 ms or more (a power-down then power-up; a strobe pulse is a few
   microseconds). Short absences -- a disk transaction -- are not power-ups,
   so the deep FIFO applies at once on those re-entries.

**Not verified, and what to watch for:**
- The warm-up rule is an inference from Wolfenstein 3D's detection code and
  the DSS guide. A program that detects mid-run (no 1 s absence, no pin-17
  power cycle) would fail detection at a deep depth: set 16 first
  (`DSSCFG default`). Other games may detect differently.
- Latency rises by up to depth/7000 s plus what the flush queues, and a
  program that sequences on "sample finished" will see it finish early by the
  amount buffered, since its data is consumed faster than real time during a
  load.
- `DSSSTRES` caps its feeder at 32 writes per timer interrupt (a safety
  limit), so it does not exercise a deep FIFO; use `DSSTEST detect`, which
  should still pass at any depth (run it after the port has been idle for a
  second so it counts as a fresh power-up) and a real game.
- The ESC diagnostics show `FIFO depth: configured=... now ... warm-ups=...
  flushed to DAC=...`. `flushed to DAC` climbing during a game's disk loads
  is the feature working.
- The command's byte protocol is unit-tested on a host against a mock wire
  layer (`firmware/tests/test_dss_cmd.c`, including that every byte value
  survives the Busy-line inversion); the model, the warm-up policy and the
  power tracker are unit-tested too. None of it has run on the board.

## 7. What is and is not confirmed

Confirmed here (no hardware): `dss_model.c` passes
`firmware/tests/run_tests.sh` (FIFO depth/overflow, full-flag timing,
7 kHz drain, hold-when-empty, no back-fill on resume, lateness cap,
paced-writer rate); the firmware builds and links against pico-sdk 2.3.0
with no warnings and the PIO program assembles; `DSSTEST.EXE` builds
under BC31 (default 8086 code generation, no warnings).

Also confirmed, from the Wolf3D source (`ID_SD.C`): the game strobes with
`ssOn = 04h` / `ssOff = 0Ch` (`0Eh` only in Tandy mode) and tests status
bit 6 (`0x40`) for FIFO-full, exactly the sequence this firmware keys on.

**Not** confirmed without hardware:

- That the PIO capture sees the pins correctly after `pio_gpio_init()`
  moves them to PIO1 while `wire.c` keeps reading them with
  `gpio_get()`. This follows the SDK's own input-pin convention, but
  watch the pin states in the ESC diagnostics on first boot.
- Strobe edge quality on pin 17 through the MOSFET level shifter at
  1-10 us pulses. Worth a scope trace.
- nAck "full" latency vs. a fast host (CPU polling loop, sub-microsecond
  by design, not measured). `overflow(dropped)` exposes it.
- Behaviour on a real 4.77 MHz XT: strobe low-time, ISR interaction,
  and the code actually executing on an 8088.
