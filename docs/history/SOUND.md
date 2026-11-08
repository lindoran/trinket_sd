# PCM5102 High quality sound DAC dispatch notes

### How its Connected:

The module is connected to the prototype across the two available headers.

| PIN    | Module Signal | Note                            |
|--------|---------------|---------------------------------|
| 16     | BCK           | Bit Clock                       |
| 17     | LRCK          | Left / Right Clock / WS         |
| 27     | DIN           | Data in                         | 
| 26     | SCLK          | System Clock (we will trigger with GPIO) |
| 3.3V   | VIN           | Voltage source from RP Pico On board regulator |
| GND    | GND           | Self evident |

None of the bridges are set on the back of the module, so its addressing 
is based off of that. 

we can always ground SCLK through the MCU, based on configured settings
in the dispatch to use the internal clock, this way we can use very slow
low quality sample rates to meet compatibility with legacy samples.

we have removed the pull-ups from the prototype and connected directly 
to the pins as the current module has all this accounted for in its design.
These pins are meant to be connected this way.  All documents in the repo.
have been updated to reflect this.

### Bring up goals for the time being.

we want to set up a test jig to do the following:

1) Prevent interference from in transit data transfers on the storage device, so that a data operation can not be broken.  This may require a way for the dispatch system to multiplex traffic.
2) A way to test this multiplexed traffic.
3) implement the full set of commands the chip can take, and handle the overhead on the pico through the dispatch.
4) play a beep using the internal oscillator.
5) play a beep using the clocked oscillator on pin 26 which should be configurable through the dispatch.

This should be done with a IA16 c program, this program needs to function so the dos driver 
does not break for the SD card.  Once this is done we can move on to talking about device drivers if any



