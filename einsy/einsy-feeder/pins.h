// SPDX-License-Identifier: MIT
// Einsy RAMBo (Prusa MK3 / MK3S) pin map, as Arduino Mega 2560 pin numbers.
//
// Sources:
//  [1] Prusa-Firmware, branch MK3, Firmware/pins_Einsy_1_0.h
//      https://github.com/prusa3d/Prusa-Firmware/blob/MK3/Firmware/pins_Einsy_1_0.h
//      Its numbers are Arduino Mega numbers: they match the Mega pin table
//      (3 = PE5, 4 = PG5, 6 = PH3, 37 = PC0, 64 = A10 ...).
//  [2] UltiMachine Einsy-Rambo schematic, tag/branch 1.1a, sheets 3 (MCU),
//      5 (Expansion Connectors), 7 (Heaters), 8-11 (Motors)
//      https://github.com/ultimachine/Einsy-Rambo (board/Project Outputs/*.PDF)
//  [3] Prusa-Firmware Firmware/uart2.cpp and variants/MK3S.h (RPi port = USART2)
//
// Confidence: HIGH for STEP/DIR/EN/CS/DIAG, heater and fan pins (the Prusa
// header and the schematic agree). HIGH for the RPi port being USART2.
// MEDIUM on the board revision: the schematic is 1.1a. The MK3 shipped 1.0a
// and 1.1a boards. The owner should check the silk screen and the voltage.
#ifndef EINSY_PINS_H
#define EINSY_PINS_H

// ---- Motors: X, Y, Z, E drivers (TMC2130 over SPI) -------------------------
// Index 0..3 = X, Y, Z, E. Augers A, B, C use X, Y, Z. The spinner uses E.
//   axis   STEP  DIR  EN(active low)  CS  DIAG1 (StallGuard, active low)
#define X_STEP_PIN 37
#define X_DIR_PIN 49
#define X_EN_PIN 29
#define X_CS_PIN 41
#define X_DIAG_PIN 64

#define Y_STEP_PIN 36
#define Y_DIR_PIN 48
#define Y_EN_PIN 28
#define Y_CS_PIN 39
#define Y_DIAG_PIN 69

#define Z_STEP_PIN 35
#define Z_DIR_PIN 47
#define Z_EN_PIN 27
#define Z_CS_PIN 67
#define Z_DIAG_PIN 68

#define E_STEP_PIN 34
#define E_DIR_PIN 43
#define E_EN_PIN 26
#define E_CS_PIN 66
#define E_DIAG_PIN 65

// TMC2130 sense resistors: 0.22 Ohm on every axis (R17/R18 and the like, [2] sheet 8).
// The CLK pin is tied to GND, so the drivers use their internal clock.
#define R_SENSE 0.22f

// ---- SPI bus (hardware SPI on the ATmega2560) -------------------------------
// SCK = 52 (PB1), MOSI = 51 (PB2), MISO = 50 (PB3). Shared with the SD card
// and, on 1.1a boards, with an external flash. Their chip selects must stay
// high or they fight the TMC2130 on MISO.
// SD card chip select = PJ6 (Prusa pin 77, SDSS in [1]). It also gates the SD MISO
// buffer (U9, [2] sheet 5). The stock arduino:avr:mega variant stops at pin 69, so
// the sketch drives it through PORTJ/DDRJ.
#define SD_CS_BIT PJ6
#define XFLASH_CS_PIN 32   // XFLASH_PIN_CS in [1] (1.1a boards)

// ---- Heaters and fans: driven LOW at boot and kept LOW ----------------------
// The feeder has no heaters. A floating gate is a fire risk, so these four
// outputs are set low first thing and never touched again. ([1], [2] sheet 3, 7)
#define HEATER_BED_PIN 4   // PG5, "BED-HEAT"
#define HEATER_0_PIN 3     // PE5, "HEAT-0" (hotend)
#define FAN_1_PIN 6        // PH3, "FAN-1" (print fan, FAN_PIN in [1])
#define FAN_0_PIN 8        // PH5, "FAN-0" (hotend fan, EXTRUDER_0_AUTO_FAN_PIN in Prusa MK3.h)
// The heater MOSFETs also pass through an AND gate with nAC_FAULT and a 100 kOhm
// pull-down at the gate input ([2] sheet 7). That is a hardware backstop, not a
// replacement for the firmware rule above.

// ---- Raspberry Pi header = P3 on the schematic, USART2 ----------------------
// Arduino Serial2: TX2 = pin 16 (PH1), RX2 = pin 17 (PH0). [2] sheet 3 and 5, [3].
// P3 (2x5 header, "Raspberry Pi" port): pin 3 = TX2 (via 100 R), pin 5 = RX2,
// pins 4 / 7 / 10 = GND, pins 1 and 2 = VCC (5 V rail). Pin 6 = SCL, 8 = SDA.
// Level: the ATmega2560 runs on 5 V (VCC) and the schematic shows no level
// shifter on TX2 / RX2: expect 5 V TTL. MEASURE before connecting the 3.3 V C3.
#define HOST_SERIAL Serial2
#define HOST_BAUD 57600UL

// ---- Optional lid switch ---------------------------------------------------
// PLACEHOLDER: disabled. To use one, set LID_SWITCH_PIN to an endstop input
// (Prusa pins: X_MIN 12, Y_MIN 11, Z_MIN 10, Z_MAX 23) and wire the lid reed
// switch to GND. The firmware uses the internal pull-up: lid open = pin HIGH.
#define LID_SWITCH_PIN -1

#endif  // EINSY_PINS_H
