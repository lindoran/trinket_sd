/*
 * subsys_dac.h -- High-quality I2S Audio DAC subsystem for PCM5102 on RP2040.
 *
 * Selected via DISPATCH_CMD_DAC (0x04).
 *
 * Hardware Pin Mapping (SOUND.md):
 *   PIN 16 (GPIO16): BCK  (Bit Clock)
 *   PIN 17 (GPIO17): LRCK (Left / Right Clock / Word Select)
 *   PIN 27 (GPIO27): DIN  (Data In)
 *   PIN 26 (GPIO26): SCLK (System Clock / Master Clock, software-configurable)
 *   3.3V:            VIN  (Voltage source from RP2040 onboard regulator)
 *   GND:             GND
 *
 * SCLK Operation:
 *   - DAC_SCLK_INTERNAL (0x00): GPIO26 is held LOW by the MCU. The PCM5102
 *     automatically detects SCLK=0 and uses its internal PLL to generate
 *     system clock from BCK (3-wire I2S mode).
 *   - DAC_SCLK_EXTERNAL (0x01): GPIO26 generates a 256*Fs master clock via
 *     RP2040 PWM slice 5.
 *
 * Streaming Architecture:
 *   Audio playback is fully asynchronous and hardware-driven (PIO + DMA).
 *   The CPU does not bit-bang or wait for individual audio samples.
 *   Audio plays continuously in the background while the RP2040 services
 *   parallel-port wire commands and SD storage requests concurrently.
 *
 * Protocol framing (DISPATCH_CMD_DAC):
 *   WRITE cycle: Opcode (DAC_OP_*) followed by op-specific arguments/payload.
 *     DAC_OP_CONFIG:        sample_rate(4,LE), channels(1), bits(1), sclk_mode(1)
 *     DAC_OP_START:         no payload
 *     DAC_OP_STOP:          no payload
 *     DAC_OP_STATUS:        no payload
 *     DAC_OP_WRITE:         count(2,LE), count bytes of PCM payload -- format
 *                            depends on the `bits` most recently set via
 *                            DAC_OP_CONFIG (default 16 if never configured):
 *                              bits=16: 16-bit LE PCM, mono or stereo-
 *                                interleaved per current `channels`
 *                              bits=8:  unsigned 8-bit PCM (midpoint 128,
 *                                standard WAV/raw convention), mono or
 *                                stereo-interleaved per current `channels` --
 *                                upsampled to 16-bit on ingestion (see
 *                                subsys_dac.c's upsample_8_to_16()) so the
 *                                ring buffer/DMA/PIO path downstream never
 *                                has to know the source was 8-bit. Half the
 *                                wire bytes per sample vs bits=16, added
 *                                2026-09 to test whether parallel-port wire
 *                                bandwidth is what causes playback underruns
 *                                at higher rates/bit depths.
 *     DAC_OP_TONE:          freq_hz(2,LE), duration_ms(2,LE), sclk_mode(1), sample_rate(4,LE)
 *     DAC_OP_RESET_METRICS: no payload
 *
 *   READ cycle: 21-byte status block:
 *     status(1), state(1), sample_rate(4,LE), channels(1), bits(1), sclk_mode(1),
 *     ring_free(2,LE), ring_queued(2,LE), underrun_count(4,LE), frames_played(4,LE).
 */

#ifndef TRINKET_SUBSYS_DAC_H
#define TRINKET_SUBSYS_DAC_H

#include <stdint.h>
#include <stdbool.h>

#define DISPATCH_CMD_DAC      0x04u

/* DAC Operations (Write direction opcode) */
#define DAC_OP_CONFIG         0x01u  /* sample_rate(4,LE), channels(1), bits(1), sclk_mode(1) */
#define DAC_OP_START          0x02u  /* Start playback */
#define DAC_OP_STOP           0x03u  /* Stop playback / pause */
#define DAC_OP_STATUS         0x04u  /* Query status */
#define DAC_OP_WRITE          0x05u  /* Write PCM sample data */
#define DAC_OP_TONE           0x06u  /* Play hardware test tone / beep */
#define DAC_OP_RESET_METRICS  0x07u  /* Reset underrun & frame counters */

/* SCLK Modes */
#define DAC_SCLK_INTERNAL     0x00u  /* GPIO26 driven LOW -> PCM5102 internal PLL */
#define DAC_SCLK_EXTERNAL     0x01u  /* GPIO26 driven with 256*Fs clock via PWM */

/* Status Byte (Read direction response) */
#define DAC_STATUS_OK         0x00u
#define DAC_STATUS_ERROR      0x01u

/* Playback States */
#define DAC_STATE_IDLE        0x00u
#define DAC_STATE_PLAYING     0x01u

/* Initialization & Lifecycle */
void subsys_dac_init(void);
void subsys_dac_reset(void);
bool subsys_dac_run(void);
void subsys_dac_service(void);
void subsys_dac_set_debug(bool enabled);

/* Disney Sound Source emulation sink (subsys_dss.c) -- NOT part of the
 * DISPATCH_CMD_DAC wire protocol; firmware-internal only.
 *
 *   begin: set the I2S rate, mark the stream live (so a starved ring is
 *          counted as an underrun again). Does not flush the ring.
 *   push:  append n UNSIGNED 8-bit mono samples (midpoint 128) to the
 *          ring as stereo 16-bit frames, same conversion as DAC_OP_WRITE
 *          in 8-bit mono mode. Never blocks: if the ring has no room the
 *          excess samples are dropped and counted (see _drops), because
 *          the caller is a timing-critical loop that must keep servicing
 *          the LPT strobe capture.
 *   end:   clears the stream flag; queued audio still plays out.
 */
void subsys_dac_dss_begin(uint32_t rate_hz);
void subsys_dac_dss_push(const uint8_t *samples, uint32_t n);
void subsys_dac_dss_end(void);
uint32_t subsys_dac_dss_drops(void);          /* samples dropped: ring full */
uint32_t subsys_dac_ring_queued_frames(void); /* stereo frames waiting in the ring */
uint32_t subsys_dac_ring_free_frames(void);   /* stereo frames of room left in the ring */
uint32_t subsys_dac_underruns(void);          /* same counter DAC_OP_STATUS reports */

/* Hardware Diagnostic & Tone Functions */
void subsys_dac_play_tone(uint16_t freq_hz, uint16_t duration_ms, uint8_t sclk_mode);
void subsys_dac_dump_diagnostics(void);

#endif /* TRINKET_SUBSYS_DAC_H */
