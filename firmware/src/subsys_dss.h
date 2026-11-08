/*
 * subsys_dss.h -- Disney Sound Source emulation.
 *
 * Unlike every other subsystem here this is NOT reached through
 * DISPATCH_CMD_*: there is no command byte and no Init/Error handshake.
 * A DSS-aware program (a game that already knows how to talk to a real
 * Disney Sound Source on the parallel port) writes the port directly --
 * data on D0-D7, a low pulse on pin 17 whose rising edge clocks the byte
 * in, and it polls nAck (pin 10) for "FIFO full". The trinket sits on
 * exactly those wires, so it can simply BE the Sound Source: no host-
 * side software, works on an 8088, works on unmodified games.
 *
 * How it is recognised
 * --------------------
 * The DSS Programmer's Guide's reference routine writes the control
 * port as 04h (power-up) and 0Ch (strobe low / power-down). Bit 1 clear
 * -> pin 14 HIGH; bit 2 set -> pin 16 HIGH. The trinket's own protocol
 * never has both high at once (wire_dss_signature() in wire.h has the
 * argument), so while both ARE high the firmware stops listening for
 * dispatch commands and runs a DSS session (subsys_dss_run_session())
 * instead. When either line drops the session ends and the dispatcher
 * resumes. main.c owns that decision.
 *
 * What a session does
 * -------------------
 *   - PIO (dss_capture.pio) latches D0-D7 on each strobe rising edge.
 *   - dss_model.c is the 16-deep FIFO + 7kHz drain clock; the nAck
 *     output (GPIO19) follows its full flag.
 *   - Every drain tick emits one sample (the popped byte, or the held
 *     last value when the FIFO ran dry) into the same DAC ring buffer
 *     the wire protocol feeds, so audio goes out through the existing
 *     PCM5102 path with no second code path.
 *
 * Known limits (see DSS.md): control values with bit 1 set while
 * strobing (the Tandy variant, 0Eh) are not recognised; the PIO input
 * pins have not been verified on hardware yet; audio is delayed ~0.1s
 * by the DMA chunk size.
 */

#ifndef TRINKET_SUBSYS_DSS_H
#define TRINKET_SUBSYS_DSS_H

#include <stdbool.h>

/* Call once at boot, after wire_setup_pins(): loads the PIO capture
 * program on pio1 and claims a state machine. */
void subsys_dss_init(void);

/* Clears the FIFO model. main.c calls this with the other subsystems'
 * resets when a real host reset completes. Statistics survive. */
void subsys_dss_reset(void);

/* Blocks for as long as wire_dss_signature() stays true, servicing the
 * strobe capture, the FIFO-full line and the DAC feed. On return the
 * wire outputs are parked; if the host left by asserting reset instead
 * of releasing nAuto-LF, the reset-pending flag has been re-armed so
 * main.c's next loop iteration runs the normal reset sequence. */
void subsys_dss_run_session(void);

/* DISPATCH_CMD_DSS handler: configures the FIFO depth the emulation
 * presents. Same shape as the SD/DAC subsystems -- after the command byte,
 * direction WRITE sends an op, direction READ returns the current config:
 *
 *   DSS_OP_SET (0x01) + depth_lo depth_hi warmup_lo warmup_hi
 *       depth      16..512 (clamped); 16 = the real unit, the default
 *       warmup_ms  0..2000 (clamped); how long a freshly powered-up program
 *                  sees the real 16-byte FIFO before the configured depth
 *                  takes over, so presence tests that overrun the FIFO and
 *                  expect "full" still pass
 *   DSS_OP_GET (0x02)   no arguments; the READ that follows returns it
 *
 *   READ result, 7 bytes: status (0), depth_lo, depth_hi, warmup_lo,
 *   warmup_hi, max_depth_lo, max_depth_hi -- the values actually in force
 *   after clamping.
 *
 * The setting lives in RAM: a Pico power-cycle returns it to 16/250, so
 * TRINKET.SYS re-sends it on every card initialisation (its /d and /w
 * switches). A host reset does NOT clear it. */
#include "dss_cmd.h"      /* DSS_OP_SET / DSS_OP_GET */
bool subsys_dss_cmd_run(void);

void subsys_dss_set_debug(bool enabled);
void subsys_dss_dump_diagnostics(void);

#endif /* TRINKET_SUBSYS_DSS_H */
