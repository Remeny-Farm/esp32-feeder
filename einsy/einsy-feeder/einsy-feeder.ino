// SPDX-License-Identifier: MIT
// Einsy RAMBo (ATmega2560) motor controller for the chicken treat feeder.
//
// The ESP32-C3 sends commands over UART (see einsy/PROTOCOL.md). This firmware
// is the SECOND safety layer: it stays safe even if the C3 misbehaves.
//
// Safety rules in this file:
//  1. Heaters and fans are driven LOW at reset and never touched again.
//  2. Drivers stay disabled (EN high, TOFF = 0) until a valid command.
//  3. Hard limits are compiled in (feeder_limits.h). Over-limit commands are
//     rejected, never clamped. The step generator ISR enforces them again, so
//     a hung main loop still stops the motor in bounded time.
//  4. No valid line for HEARTBEAT_TIMEOUT_MS: stop all, disable drivers, fault.
//  5. The AVR hardware watchdog resets a hung board into the safe state.
//  6. Only one auger moves at a time. The spinner may run alongside.
//  7. StallGuard stops an auger and latches a fault.
//
// Target: arduino:avr:mega. Library: TMCStepper (MIT).

#include <SPI.h>
#include <TMCStepper.h>
#include <avr/interrupt.h>
#include <avr/io.h>
#include <avr/wdt.h>
#include <stdarg.h>
#include <stdio.h>

#include "feeder_limits.h"
#include "pins.h"
#include "protocol.h"

// ---------------------------------------------------------------------------
// Reset-time safety. This runs in .init3, before main() and before the Arduino
// core, so the heater and fan pins are low as early as the chip allows.
// It also stops a watchdog that the bootloader left running, which would
// otherwise reset the chip in a loop.
// ---------------------------------------------------------------------------
static uint8_t g_resetCause __attribute__((section(".noinit")));

extern "C" void early_safe_init(void) __attribute__((naked, used, section(".init3")));
extern "C" void early_safe_init(void) {
  g_resetCause = MCUSR;
  MCUSR = 0;
  wdt_disable();
  // Heater/fan pins: PE5 (3), PG5 (4), PH3 (6), PH5 (8). Latch low, then output.
  PORTE &= (uint8_t)~_BV(PE5);
  PORTG &= (uint8_t)~_BV(PG5);
  PORTH &= (uint8_t) ~(_BV(PH3) | _BV(PH5));
  DDRE |= _BV(PE5);
  DDRG |= _BV(PG5);
  DDRH |= _BV(PH3) | _BV(PH5);
}

// ---------------------------------------------------------------------------
// Hardware tables
// ---------------------------------------------------------------------------
struct AxisPins {
  uint8_t step, dir, en, cs, diag;
};

// Index 0..2 = X, Y, Z = augers A, B, C. Index 3 = E = spinner.
static const AxisPins AX[4] = {
    {X_STEP_PIN, X_DIR_PIN, X_EN_PIN, X_CS_PIN, X_DIAG_PIN},
    {Y_STEP_PIN, Y_DIR_PIN, Y_EN_PIN, Y_CS_PIN, Y_DIAG_PIN},
    {Z_STEP_PIN, Z_DIR_PIN, Z_EN_PIN, Z_CS_PIN, Z_DIAG_PIN},
    {E_STEP_PIN, E_DIR_PIN, E_EN_PIN, E_CS_PIN, E_DIAG_PIN},
};
static const uint8_t SPINNER = 3;

// DIR pin level that feeds forward. PLACEHOLDER: check on the real augers.
static const uint8_t FEED_DIR[4] = {LOW, LOW, LOW, LOW};
// Motor current per axis, RMS mA. PLACEHOLDER: see feeder_limits.h.
static const uint16_t CURRENT_MA[4] = {AUGER_CURRENT_MA, AUGER_CURRENT_MA, AUGER_CURRENT_MA,
                                       SPINNER_CURRENT_MA};

static TMC2130Stepper drvX(X_CS_PIN, R_SENSE);
static TMC2130Stepper drvY(Y_CS_PIN, R_SENSE);
static TMC2130Stepper drvZ(Z_CS_PIN, R_SENSE);
static TMC2130Stepper drvE(E_CS_PIN, R_SENSE);
static TMC2130Stepper* const drv[4] = {&drvX, &drvY, &drvZ, &drvE};
static bool drvOk[4];  // TMC2130 answered with the right chip version

// ---------------------------------------------------------------------------
// Step generator: Timer1 ISR at TICK_HZ with a phase accumulator per channel.
// Channel 0 = the moving auger, channel 1 = the spinner.
// The ISR owns the step pin and enforces step count, run time, a rate guard and
// StallGuard. The main loop only sets the speed.
// ---------------------------------------------------------------------------
enum EndReason : uint8_t { END_NONE = 0, END_DONE, END_TIME, END_STALL, END_RATE };

struct Chan {
  volatile uint8_t* stepPort;
  uint8_t stepMask;
  volatile uint8_t* diagIn;
  uint8_t diagMask;  // 0 = no stall input (spinner)
  volatile bool active;
  volatile bool pulse;
  volatile uint32_t acc;
  volatile uint32_t rate;       // steps per second
  volatile uint32_t remaining;  // steps left
  volatile uint32_t done;       // steps made
  volatile uint32_t ticksLeft;  // run-time budget
  volatile uint32_t blank;      // ticks before StallGuard counts
  volatile uint8_t stallCnt;
  volatile uint8_t endReason;
};
static Chan ch[2];
static const uint32_t TICKS_PER_MS = TICK_HZ / 1000UL;
static const uint32_t MAX_RATE[2] = {MAX_DOSE_SPEED, MAX_SPIN_SPEED};
static const uint8_t STALL_TICKS = 5;  // consecutive low DIAG samples = stall

ISR(TIMER1_COMPA_vect) {
  for (uint8_t i = 0; i < 2; i++) {
    Chan& c = ch[i];
    if (c.pulse) {  // end the step pulse one tick (100 us) after it began
      *c.stepPort &= (uint8_t)~c.stepMask;
      c.pulse = false;
    }
    if (!c.active) continue;

    uint32_t rate = c.rate;
    if (rate > MAX_RATE[i] || rate == 0) {  // rate guard: corrupted state
      c.endReason = END_RATE;
      c.active = false;
      continue;
    }
    if (c.blank) {
      c.blank--;
    } else if (c.diagMask) {
      if (!(*c.diagIn & c.diagMask)) {  // DIAG1 is active low (open collector)
        if (++c.stallCnt >= STALL_TICKS) {
          c.endReason = END_STALL;
          c.active = false;
          continue;
        }
      } else {
        c.stallCnt = 0;
      }
    }

    uint32_t acc = c.acc + rate;
    if (acc >= TICK_HZ) {
      acc -= TICK_HZ;
      *c.stepPort |= c.stepMask;
      c.pulse = true;
      c.done++;
      if (--c.remaining == 0) {
        c.acc = acc;
        c.endReason = END_DONE;
        c.active = false;
        continue;
      }
    }
    c.acc = acc;

    if (--c.ticksLeft == 0) {
      c.endReason = END_TIME;
      c.active = false;
    }
  }
}

static void timerStart() {
  noInterrupts();
  TCCR1A = 0;  // normal port operation: no PWM on OC1x pins
  TCCR1B = _BV(WGM12) | _BV(CS11);  // CTC, clock / 8 = 2 MHz
  OCR1A = (uint16_t)(F_CPU / 8UL / TICK_HZ - 1);
  TCNT1 = 0;
  TIMSK1 = _BV(OCIE1A);
  interrupts();
}

// ---------------------------------------------------------------------------
// Replies
// ---------------------------------------------------------------------------
static void sendBody(const char* body) {
  char buf[170];
  size_t n = proto::format_line(buf, sizeof(buf), body);
  if (n) HOST_SERIAL.write((const uint8_t*)buf, n);
}

static void sendf(const char* fmt, ...) {
  char body[150];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(body, sizeof(body), fmt, ap);
  va_end(ap);
  sendBody(body);
}

static void replyOk(uint16_t seq) { sendf("OK %u", (unsigned)seq); }
static void replyErr(uint16_t seq, proto::Err e) {
  sendf("ERR %u %s", (unsigned)seq, proto::err_name(e));
}

// ---------------------------------------------------------------------------
// Driver control
// ---------------------------------------------------------------------------
static char axisChar(uint8_t idx) { return idx == SPINNER ? 'S' : (char)('A' + idx); }

static void driverDisable(uint8_t idx) {
  digitalWrite(AX[idx].en, HIGH);  // active low: HIGH = outputs off
  if (drvOk[idx]) drv[idx]->toff(0);  // chopper off too (second way to disable)
}

// Write the whole configuration. Called at boot and before every move, so a
// driver that reset by itself never runs with stale settings. EN stays high.
static void driverConfigure(uint8_t idx) {
  TMC2130Stepper& d = *drv[idx];
  d.begin();  // writes the cached registers; leaves TOFF = 8
  d.toff(0);  // driver stays off until driverEnable()
  digitalWrite(AX[idx].en, HIGH);
  d.GSTAT(0x07);  // clear reset / drv_err / uv_cp flags
  d.I_scale_analog(false);  // current from the SPI setting, not the AIN pin
  d.internal_Rsense(false);
  d.tbl(2);
  d.hstrt(5);
  d.hend(2);
  d.rms_current(CURRENT_MA[idx], HOLD_MULTIPLIER);
  d.microsteps(MICROSTEPS);
  d.intpol(true);
  d.TPOWERDOWN(20);
  if (idx == SPINNER) {
    d.en_pwm_mode(SPINNER_STEALTHCHOP != 0);
    d.pwm_autoscale(true);
    d.TPWMTHRS(0);
    d.diag1_stall(false);
  } else {
    // spreadCycle: StallGuard needs it. DIAG1 is open collector, active low,
    // pulled up on the Einsy (10 kOhm on the shared DIAG net).
    d.en_pwm_mode(false);
    d.TCOOLTHRS(0xFFFFFUL);
    d.sgt(STALLGUARD_SGT);
    d.sfilt(true);
    d.diag1_pushpull(false);
    d.diag1_stall(true);
  }
  drvOk[idx] = (d.version() == 0x11);  // TMC2130 IOIN version
}

static void driverEnable(uint8_t idx) {
  digitalWrite(AX[idx].dir, FEED_DIR[idx]);
  drv[idx]->toff(3);
  digitalWrite(AX[idx].en, LOW);
}

static bool lidOpen() {
#if LID_SWITCH_PIN >= 0
  return digitalRead(LID_SWITCH_PIN) == HIGH;
#else
  return false;
#endif
}

// ---------------------------------------------------------------------------
// Jobs and fault state
// ---------------------------------------------------------------------------
struct Job {
  bool running;
  bool pendingDone;  // a DONE line is still to be sent
  uint16_t seq;
  uint8_t idx;
  uint32_t startMs;
  uint32_t target;  // target speed, steps per second
  uint32_t lastPollMs;
  uint32_t stepsDone;
};
static Job jobs[2];  // 0 = auger, 1 = spinner

static bool faulted = false;
static const char* faultName = "none";
static char faultAxis = '-';
static bool seenValid = false;  // first valid line arms the heartbeat watchdog
static uint32_t lastValidMs = 0;
static bool haveLastSeq = false;
static uint16_t lastActionSeq = 0;
static uint32_t lastBadCrcMs = 0;
static bool badCrcEver = false;

// Stop one job now. The DONE line is sent later by flushDone().
static void endJob(uint8_t j) {
  Job& jb = jobs[j];
  if (!jb.running) return;
  Chan& c = ch[j];
  noInterrupts();
  c.active = false;
  uint32_t done = c.done;
  interrupts();
  // The ISR clears a pulse that is still high on its next tick.
  jb.stepsDone = done;
  jb.running = false;
  jb.pendingDone = true;
  driverDisable(jb.idx);
}

static void flushDone() {
  for (uint8_t j = 0; j < 2; j++) {
    if (jobs[j].pendingDone) {
      jobs[j].pendingDone = false;
      sendf("DONE %u %lu", (unsigned)jobs[j].seq, (unsigned long)jobs[j].stepsDone);
    }
  }
}

static void stopAll() {
  endJob(0);
  endJob(1);
}

// Latch a fault: stop everything, disable the drivers, report once.
static void raiseFault(const char* name, char axis) {
  stopAll();
  for (uint8_t i = 0; i < 4; i++) digitalWrite(AX[i].en, HIGH);  // belt and braces
  if (!faulted) {
    faulted = true;
    faultName = name;
    faultAxis = axis;
    sendf("FAULT %s %c", name, axis);
  }
  flushDone();
}

static void startChannel(uint8_t j, uint8_t idx, uint32_t steps, uint32_t ticks,
                         uint32_t startRate, uint32_t blankTicks) {
  Chan& c = ch[j];
  c.active = false;
  while (c.pulse) {
  }  // the ISR ends the last step pulse within one tick
  c.stepPort = portOutputRegister(digitalPinToPort(AX[idx].step));
  c.stepMask = digitalPinToBitMask(AX[idx].step);
  c.diagIn = portInputRegister(digitalPinToPort(AX[idx].diag));
  c.diagMask = (j == 0) ? digitalPinToBitMask(AX[idx].diag) : 0;
  c.acc = 0;
  c.rate = startRate;
  c.remaining = steps;
  c.done = 0;
  c.ticksLeft = ticks;
  c.blank = blankTicks;
  c.stallCnt = 0;
  c.endReason = END_NONE;
  c.active = true;
}

// Linear ramp from START_SPEED to the target over RAMP_MS.
// (Plain parameters, not a Job: the Arduino prototype generator needs that.)
static uint32_t rampRate(uint32_t startMs, uint32_t target, uint32_t now) {
  uint32_t start = START_SPEED < target ? START_SPEED : target;
  uint32_t el = now - startMs;
  if (el >= RAMP_MS) return target;
  return start + (target - start) * el / RAMP_MS;
}

static void beginJob(uint8_t j, uint16_t seq, uint8_t idx, uint32_t target) {
  Job& jb = jobs[j];
  jb.running = true;
  jb.pendingDone = false;
  jb.seq = seq;
  jb.idx = idx;
  jb.startMs = millis();
  jb.target = target;
  jb.lastPollMs = jb.startMs;
  jb.stepsDone = 0;
}

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------
static void cmdDose(const proto::Command& cmd) {
  uint8_t idx = (uint8_t)(cmd.axis - 'A');
  if (haveLastSeq && cmd.seq == lastActionSeq) return replyErr(cmd.seq, proto::E_DUP);
  if (faulted) return replyErr(cmd.seq, proto::E_FAULT);
  if (jobs[0].running) return replyErr(cmd.seq, proto::E_BUSY);  // one auger at a time
  if (lidOpen()) return replyErr(cmd.seq, proto::E_LID);
  driverConfigure(idx);
  if (!drvOk[idx]) {
    digitalWrite(AX[idx].en, HIGH);
    return replyErr(cmd.seq, proto::E_DRIVER);
  }
  haveLastSeq = true;
  lastActionSeq = cmd.seq;
  beginJob(0, cmd.seq, idx, cmd.speed);
  driverEnable(idx);
  delay(2);  // let the coil currents settle before the first step
  jobs[0].startMs = millis();
  uint32_t start = START_SPEED < cmd.speed ? START_SPEED : cmd.speed;
  startChannel(0, idx, cmd.steps, (MAX_RUN_MS + 2 * RAMP_MS) * TICKS_PER_MS, start,
               (STALL_BLANK_MS + RAMP_MS) * TICKS_PER_MS);
  replyOk(cmd.seq);
}

static void cmdSpin(const proto::Command& cmd) {
  if (haveLastSeq && cmd.seq == lastActionSeq) return replyErr(cmd.seq, proto::E_DUP);
  if (faulted) return replyErr(cmd.seq, proto::E_FAULT);
  if (jobs[1].running) return replyErr(cmd.seq, proto::E_BUSY);
  if (lidOpen()) return replyErr(cmd.seq, proto::E_LID);
  driverConfigure(SPINNER);
  if (!drvOk[SPINNER]) {
    digitalWrite(AX[SPINNER].en, HIGH);
    return replyErr(cmd.seq, proto::E_DRIVER);
  }
  haveLastSeq = true;
  lastActionSeq = cmd.seq;
  beginJob(1, cmd.seq, SPINNER, cmd.speed);
  driverEnable(SPINNER);
  delay(2);
  jobs[1].startMs = millis();
  uint32_t start = START_SPEED < cmd.speed ? START_SPEED : cmd.speed;
  // The time budget is exactly the requested duration. Ending on time is normal.
  startChannel(1, SPINNER, 0xFFFFFFFFUL, cmd.ms * TICKS_PER_MS, start, 0);
  replyOk(cmd.seq);
}

static const char* stateName() {
  if (faulted) return "fault";
  if (jobs[0].running && jobs[1].running) return "dose+spin";
  if (jobs[0].running) return "dose";
  if (jobs[1].running) return "spin";
  return "idle";
}

static void cmdCfg(uint16_t seq) {
  sendf("CFG %u fw=%s proto=%d st=%s fault=%s drv=%d%d%d%d ma=%u,%u maxst=%lu maxdsp=%lu "
        "maxssp=%lu maxsms=%lu maxrun=%lu hb=%lu",
        (unsigned)seq, FW_VERSION, PROTO_VERSION, stateName(), faultName, drvOk[0], drvOk[1],
        drvOk[2], drvOk[3], (unsigned)AUGER_CURRENT_MA, (unsigned)SPINNER_CURRENT_MA,
        (unsigned long)MAX_DOSE_STEPS, (unsigned long)MAX_DOSE_SPEED,
        (unsigned long)MAX_SPIN_SPEED, (unsigned long)MAX_SPIN_MS, (unsigned long)MAX_RUN_MS,
        (unsigned long)HEARTBEAT_TIMEOUT_MS);
}

static void handleLine(const char* line) {
  proto::Command cmd;
  proto::Err e = proto::parse_line(line, &cmd);
  uint32_t now = millis();
  if (e == proto::E_BAD_CRC) {
    // Line noise. It does not count as a heartbeat and never starts a motor.
    if (!badCrcEver || now - lastBadCrcMs >= BAD_CRC_REPORT_MS) {
      badCrcEver = true;
      lastBadCrcMs = now;
      sendf("FAULT bad_crc -");
    }
    return;
  }
  if (cmd.cmd != proto::CMD_NONE) {  // checksum good and command known
    lastValidMs = now;
    seenValid = true;
  }
  if (e != proto::E_OK) return replyErr(cmd.seq, e);

  switch (cmd.cmd) {
    case proto::CMD_HB:
      replyOk(cmd.seq);
      break;
    case proto::CMD_CFG:
      cmdCfg(cmd.seq);
      break;
    case proto::CMD_STOP:
      stopAll();
      replyOk(cmd.seq);
      flushDone();
      break;
    case proto::CMD_CLEAR:
      if (jobs[0].running || jobs[1].running) return replyErr(cmd.seq, proto::E_NOT_IDLE);
      faulted = false;
      faultName = "none";
      faultAxis = '-';
      replyOk(cmd.seq);
      break;
    case proto::CMD_DOSE:
      cmdDose(cmd);
      break;
    case proto::CMD_SPIN:
      cmdSpin(cmd);
      break;
    default:
      replyErr(cmd.seq, proto::E_UNKNOWN_CMD);
      break;
  }
}

// ---------------------------------------------------------------------------
// Serial input
// ---------------------------------------------------------------------------
static char rxBuf[MAX_LINE_LEN + 2];
static uint8_t rxLen = 0;
static bool rxOverflow = false;

static void pollSerial() {
  for (uint8_t n = 0; n < 32 && HOST_SERIAL.available(); n++) {
    char c = (char)HOST_SERIAL.read();
    if (c == '\n') {
      if (rxOverflow) {
        handleLine("");  // counts as a bad line
      } else {
        rxBuf[rxLen] = '\0';
        handleLine(rxBuf);
      }
      rxLen = 0;
      rxOverflow = false;
    } else if (rxLen < sizeof(rxBuf) - 1) {
      rxBuf[rxLen++] = c;
    } else {
      rxOverflow = true;
    }
  }
}

// ---------------------------------------------------------------------------
// Motion supervision (main loop)
// ---------------------------------------------------------------------------
static const uint32_t DRV_BAD_MASK = (1UL << 25) | (1UL << 27) | (1UL << 28);  // ot, s2ga, s2gb

static void superviseJob(uint8_t j, uint32_t now) {
  Job& jb = jobs[j];
  if (!jb.running) return;
  Chan& c = ch[j];

  if (!c.active) {  // the ISR ended it
    uint8_t why = c.endReason;
    char axis = axisChar(jb.idx);
    if (why == END_DONE || (why == END_TIME && j == 1)) {
      endJob(j);
      flushDone();
    } else if (why == END_STALL) {
      raiseFault("stall", axis);
    } else {  // END_TIME on an auger, END_RATE: a hard limit was hit
      raiseFault("limit", axis);
    }
    return;
  }

  if (lidOpen()) return raiseFault("limit", '-');

  uint32_t rate = rampRate(jb.startMs, jb.target, now);
  noInterrupts();
  if (c.active) c.rate = rate;
  interrupts();

  if (now - jb.lastPollMs >= DRIVER_POLL_MS) {
    jb.lastPollMs = now;
    uint32_t st = drv[jb.idx]->DRV_STATUS();
    if (st == 0xFFFFFFFFUL || (st & DRV_BAD_MASK)) raiseFault("driver", axisChar(jb.idx));
  }
}

static void superviseHeartbeat(uint32_t now) {
  if (seenValid && !faulted && (now - lastValidMs) > HEARTBEAT_TIMEOUT_MS) {
    raiseFault("watchdog", '-');
  }
}

// ---------------------------------------------------------------------------
// Arduino entry points
// ---------------------------------------------------------------------------
void setup() {
  // FIRST: heaters and fans LOW, forever. (early_safe_init() already did this
  // at reset. This repeats it with the Arduino API.)
  const uint8_t safeLow[4] = {HEATER_BED_PIN, HEATER_0_PIN, FAN_1_PIN, FAN_0_PIN};
  for (uint8_t i = 0; i < 4; i++) {
    digitalWrite(safeLow[i], LOW);
    pinMode(safeLow[i], OUTPUT);
    digitalWrite(safeLow[i], LOW);
  }

  // Deselect everything on the SPI bus and disable all drivers.
  const uint8_t csHigh[5] = {X_CS_PIN, Y_CS_PIN, Z_CS_PIN, E_CS_PIN, XFLASH_CS_PIN};
  for (uint8_t i = 0; i < 5; i++) {
    digitalWrite(csHigh[i], HIGH);
    pinMode(csHigh[i], OUTPUT);
  }
  // SD card chip select: PJ6, Prusa pin 77. The stock Mega variant has no
  // Arduino number for it, so use the port registers.
  PORTJ |= _BV(SD_CS_BIT);
  DDRJ |= _BV(SD_CS_BIT);
  for (uint8_t i = 0; i < 4; i++) {
    digitalWrite(AX[i].en, HIGH);
    pinMode(AX[i].en, OUTPUT);
    digitalWrite(AX[i].step, LOW);
    pinMode(AX[i].step, OUTPUT);
    digitalWrite(AX[i].dir, LOW);
    pinMode(AX[i].dir, OUTPUT);
    pinMode(AX[i].diag, INPUT_PULLUP);
  }
#if LID_SWITCH_PIN >= 0
  pinMode(LID_SWITCH_PIN, INPUT_PULLUP);
#endif

  wdt_enable(WDTO_1S);  // a hang from here on resets the board into the safe state
  static_assert(HW_WATCHDOG_MS == 1000UL, "wdt_enable(WDTO_1S) must match HW_WATCHDOG_MS");

  HOST_SERIAL.begin(HOST_BAUD);
  SPI.begin();
  for (uint8_t i = 0; i < 4; i++) {
    driverConfigure(i);
    driverDisable(i);
    wdt_reset();
  }
  timerStart();

  const char* cause = (g_resetCause & _BV(WDRF))    ? "wdt"
                      : (g_resetCause & _BV(BORF))  ? "brownout"
                      : (g_resetCause & _BV(EXTRF)) ? "ext"
                      : (g_resetCause & _BV(PORF))  ? "power"
                                                    : "other";
  sendf("BOOT %s", cause);
}

void loop() {
  wdt_reset();
  pollSerial();
  uint32_t now = millis();
  superviseJob(0, now);
  superviseJob(1, now);
  superviseHeartbeat(now);
}
