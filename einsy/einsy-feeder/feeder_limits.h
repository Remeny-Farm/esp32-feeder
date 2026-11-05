// SPDX-License-Identifier: MIT
// Hard limits and motor settings for the Einsy feeder firmware.
//
// Every value marked PLACEHOLDER is a guess that keeps the motors gentle. The
// real values come from calibration on the real hardware. Commands above the
// limits are rejected (never clamped), so a wrong value can only be too strict.
//
// This file has no Arduino dependency: the host test includes it too.
#ifndef EINSY_FEEDER_LIMITS_H
#define EINSY_FEEDER_LIMITS_H

#define FW_VERSION "0.1.0"
#define PROTO_VERSION 1

// ---- Step units ------------------------------------------------------------
// All "steps" in the protocol are microsteps. A 200 step/rev motor at 16
// microsteps gives 3200 steps per revolution.
#define MICROSTEPS 16

// ---- Hard limits (PLACEHOLDER until calibration) ---------------------------
#define MAX_DOSE_STEPS     16000UL  // PLACEHOLDER: max steps per DOSE (5 rev)
#define MIN_DOSE_SPEED     100UL    // PLACEHOLDER: min DOSE speed, steps/s
#define MAX_DOSE_SPEED     2000UL   // PLACEHOLDER: max DOSE speed, steps/s
#define MIN_SPIN_SPEED     100UL    // PLACEHOLDER: min SPIN speed, steps/s
#define MAX_SPIN_SPEED     4000UL   // PLACEHOLDER: max SPIN speed, steps/s
#define MAX_SPIN_MS        5000UL   // PLACEHOLDER: max SPIN duration
#define MAX_RUN_MS         8000UL   // PLACEHOLDER: max run time of any command

// ---- Timing (not calibration values) ---------------------------------------
#define HEARTBEAT_TIMEOUT_MS 2000UL // no valid line for this long: stop all
#define HW_WATCHDOG_MS       1000UL  // AVR watchdog: a hang resets the board
#define RAMP_MS              300UL   // PLACEHOLDER: linear speed ramp-up time
#define START_SPEED          100UL   // PLACEHOLDER: speed at the ramp start
#define STALL_BLANK_MS       500UL   // PLACEHOLDER: ignore StallGuard after start
#define DRIVER_POLL_MS       50UL    // DRV_STATUS check interval during a move
#define BAD_CRC_REPORT_MS    500UL   // min gap between FAULT bad_crc lines

// ---- Motor currents --------------------------------------------------------
// PLACEHOLDER: the owner photographs the motor labels. Set each value to at
// most the rated phase current on the label, as RMS mA. Default is gentle.
#define AUGER_CURRENT_MA   400
#define SPINNER_CURRENT_MA 400
#define HOLD_MULTIPLIER    0.3f   // idle current factor (drivers are disabled when idle)

// ---- StallGuard (PLACEHOLDER: tune on the real augers) ----------------------
// sgt range -64..63. Lower = more sensitive. Tune with a jammed auger.
#define STALLGUARD_SGT     0

// ---- Spinner mode ----------------------------------------------------------
// 1 = stealthChop (silent), 0 = spreadCycle.
#define SPINNER_STEALTHCHOP 1

// ---- Line protocol ---------------------------------------------------------
#define MAX_LINE_LEN 60  // longest accepted line, without the newline

// Compile-time sanity checks. A bad edit of the limits fails the build.
#define TICK_HZ 10000UL  // step generator tick rate (see einsy-feeder.ino)
static_assert(MAX_DOSE_SPEED * 2 <= TICK_HZ, "MAX_DOSE_SPEED too high for TICK_HZ");
static_assert(MAX_SPIN_SPEED * 2 <= TICK_HZ, "MAX_SPIN_SPEED too high for TICK_HZ");
static_assert(MAX_DOSE_STEPS * 1000UL < 4000000000UL, "run time check would overflow");
static_assert(MAX_SPIN_MS <= MAX_RUN_MS, "MAX_SPIN_MS must not exceed MAX_RUN_MS");
static_assert(MIN_DOSE_SPEED >= 1 && MIN_SPIN_SPEED >= 1, "min speed must be > 0");
static_assert(MIN_DOSE_SPEED <= MAX_DOSE_SPEED && MIN_SPIN_SPEED <= MAX_SPIN_SPEED,
              "min speed above max speed");
static_assert(START_SPEED >= 1, "START_SPEED must be > 0");

#endif  // EINSY_FEEDER_LIMITS_H
