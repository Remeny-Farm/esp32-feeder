# Einsy motor controller (prototype)

Custom firmware for the Einsy RAMBo (ATmega2560, 4 x TMC2130) salvaged from a
Prusa i3 MK3. It is the motor controller of the prototype feeder and the
**second safety layer**: the ESP32-C3 sends commands over UART, and the Einsy
enforces its own hard limits, a heartbeat watchdog and StallGuard. See
[docs/architecture.md](../docs/architecture.md), "Prototype build".

| Driver | Motor |
|---|---|
| X, Y, Z | augers A, B, C (one moves at a time) |
| E | spinner (may run alongside an auger) |

It is not Marlin and not Prusa firmware. No heaters or thermistors exist in this
use. The four heater and fan outputs are driven LOW at reset and never touched.

Files:

| Path | Content |
|---|---|
| `einsy-feeder/einsy-feeder.ino` | sketch: step generator, supervision, command handling |
| `einsy-feeder/pins.h` | Einsy pin map with sources |
| `einsy-feeder/feeder_limits.h` | hard limits, motor currents (PLACEHOLDER values) |
| `einsy-feeder/protocol.{h,cpp}` | line parser and CRC-8 (no Arduino dependency) |
| `PROTOCOL.md` | the UART protocol |
| `test/test_protocol.cpp` | host test for the parser |

## Before you flash

**Flashing replaces the Prusa firmware.** The printer does not work as a printer
until you flash the Prusa firmware back (below). The bootloader stays. The
sketch never writes the EEPROM, so the printer settings and calibration remain.

Fill in the PLACEHOLDER values in `einsy-feeder/feeder_limits.h` first:

1. Set `AUGER_CURRENT_MA` and `SPINNER_CURRENT_MA` from the motor labels (rated
   phase current, RMS). The default is 400 mA.
2. Check the DIR level that feeds forward (`FEED_DIR` in the sketch).
3. Keep the limits strict until calibration is done.

## Build

Needs `arduino-cli`, the `arduino:avr` core and the TMCStepper library (MIT).

```sh
arduino-cli core install arduino:avr
arduino-cli lib install TMCStepper
arduino-cli compile --fqbn arduino:avr:mega --warnings all einsy/einsy-feeder
```

## Flash

The Einsy has an ATmega32U2 as USB-to-serial bridge and an optiboot-based
bootloader (Prusa). Connect the USB cable to the Einsy, find the port, then:

```sh
arduino-cli upload -p /dev/cu.usbmodemXXXX --fqbn arduino:avr:mega einsy/einsy-feeder
```

The same upload by hand, as PrusaSlicer's flasher does it (`avrdude`, programmer
`wiring`, 115200 baud, no chip erase):

```sh
avrdude -p atmega2560 -c wiring -P /dev/cu.usbmodemXXXX -b 115200 -D \
        -U flash:w:einsy-feeder.ino.hex:i
```

The Arduino Mega 2560 default settings use the same programmer and baud rate.
This was not tested on hardware: if the upload does not start, try another USB
port or cable, and see the avrdude lines in Prusa's firmware-update help page.

### Restore the Prusa firmware

1. Download the firmware for your printer (MK3S or MK3S+) from Prusa.
2. Open PrusaSlicer, then Configuration, then Flash printer firmware.
3. Select the `.hex` file and flash it. The calibration data in the EEPROM was not
   touched.

## Wiring to the ESP32-C3

Use the Einsy "Raspberry Pi" header (P3 on the Einsy schematic), which is
ATmega2560 USART2 (Arduino `Serial2`, TX2 = pin 16, RX2 = pin 17), 57600 baud.

| Einsy P3 pin | Signal | C3 |
|---|---|---|
| 3 | TX2 (Einsy sends) | RX GPIO7 (through a divider if 5 V, see below) |
| 5 | RX2 (Einsy receives) | TX GPIO6 |
| 4 (or 7, 10) | GND | GND |
| 1, 2 | VCC (5 V) | **do not connect** |

Pin numbers are from the schematic. Check them on the board with a meter first.

**Voltage level: measure before connecting.** The ATmega2560 runs on 5 V, and the
published Einsy schematic (rev 1.1a) shows no level shifter on TX2 and RX2, only
a 100 Ohm series resistor on TX2. So the header is probably **5 V TTL**, and the
C3 is 3.3 V only. Measure TX2 idle (it should read near 5 V) and then:

- Einsy TX2 to C3 RX: add a divider (for example 2.2 kOhm in series, 3.3 kOhm to
  GND) or a level shifter. Never connect 5 V directly to a C3 pin.
- C3 TX to Einsy RX2: 3.3 V is above the ATmega2560 input-high threshold at
  5 V supply (about 3.0 V), so a direct connection works. A level shifter gives
  more margin.

Use the same baud rate on both sides (`HOST_BAUD` in `pins.h`, 57600).
The C3 must send `HB` at least once per second (see `PROTOCOL.md`).

## Safety notes

- **Hardware kill.** Power the Einsy through the Power Relay FeatherWing, driven
  by the C3. The relay cuts the Einsy supply, so a firmware fault on either board
  cannot keep a motor running. The Einsy firmware is the second layer, not the
  only one.
- **Reset state is not guaranteed.** While the ATmega2560 resets (watchdog reset,
  flashing), its pins float. The Einsy schematic shows no pull-up on the driver
  EN pins, and the TMC2130 pin behavior while EN floats was not confirmed.
  Heater MOSFET gates have a 100 kOhm pull-down and an AND gate with the power-fault
  signal. If a motor twitches at reset, add a 10 kOhm pull-up from each EN pin to
  5 V. Test with the motors disconnected from the mechanics first.
- **The hard limits are placeholders.** `feeder_limits.h` limits keep the motors
  gentle, but they are guesses. Calibrate before the feeder meets the flock.
- **StallGuard needs tuning.** `STALLGUARD_SGT` is 0 and stall detection may
  trip too early at low speeds or never trip. Tune it with a jammed auger.
  The spinner has no stall detection.
- **No heaters.** Disconnect the heater cartridge and the bed. The firmware
  keeps those outputs low, and a hardware AND gate adds a second guard.
- **Faults latch.** After any fault except `bad_crc`, the Einsy refuses DOSE and
  SPIN until the C3 sends `CLEAR`.

## Host test

The parser and the checksum have no Arduino dependency:

```sh
cd einsy/test
c++ -std=c++11 -Wall -Wextra -Werror -I../einsy-feeder test_protocol.cpp \
    ../einsy-feeder/protocol.cpp -o /tmp/test_protocol && /tmp/test_protocol
```
