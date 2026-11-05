# Einsy UART line protocol

Version 1. Link: ESP32-C3 to Einsy RAMBo, USART2 (the "Raspberry Pi" header),
57600 baud, 8N1. Source of truth for the parser: `einsy-feeder/protocol.cpp`.
Limits: `einsy-feeder/feeder_limits.h`.

The Einsy is the second safety layer. Every rule below makes it safe even if
the C3 sends wrong, repeated or random lines.

## Framing

ASCII, one message per line, ended by `\n` (a `\r` before it is accepted).
A line is at most 60 characters without the newline.

```
<body>*<HH>\n
```

- `<body>` is `<seq> <COMMAND> [args...]`. Fields are separated by exactly one
  space. A double space, a leading space or a trailing space is `bad_format`.
- `<HH>` is two hex digits (upper or lower case): CRC-8 over every byte of
  `<body>`, before the `*`. CRC-8 uses polynomial 0x07, initial value 0x00, no
  reflection, no final XOR. Check value: `crc8("123456789") = 0xF4`.
- `<seq>` is a decimal number from 1 to 65535. The C3 picks it. Replies repeat
  it.
- Numbers are plain decimal digits (no sign, at most 9 digits).

A line with a wrong or missing checksum is ignored. It never starts a motor,
and it does not count as a heartbeat. The Einsy answers with a rate-limited
`FAULT bad_crc -` (at most one per 500 ms).

Every line the Einsy sends uses the same framing, so the C3 can check it too.

Example (checksums computed with an independent Python CRC-8):

```
12 DOSE A 800 400*48       C3 -> Einsy
OK 12*D8                   Einsy -> C3
DONE 12 800*58             Einsy -> C3, about 2 s later
1 HB*C6                    C3 -> Einsy, heartbeat
```

## Commands (C3 to Einsy)

| Command | Meaning |
|---|---|
| `<seq> DOSE <axis> <steps> <speed>` | Run auger `A`, `B` or `C` for `<steps>` microsteps at `<speed>` steps/s. |
| `<seq> SPIN <speed> <ms>` | Run the spinner at `<speed>` steps/s for `<ms>` milliseconds. |
| `<seq> STOP` | Stop all motors now and disable the drivers. Does not latch a fault. |
| `<seq> HB` | Heartbeat. Answered with `OK`. |
| `<seq> CFG` | Read-only status query. Changes nothing. |
| `<seq> CLEAR` | Clear a latched fault. Only when no motor runs. |

Steps are microsteps (`MICROSTEPS` = 16, so 3200 steps per revolution of a
200 step motor). Speeds are in steps per second.

Auger `A`, `B`, `C` are the X, Y, Z drivers. The spinner is the E driver.
Only one auger moves at a time. The spinner may run alongside an auger.

## Replies (Einsy to C3)

| Line | Meaning |
|---|---|
| `OK <seq>` | The command was accepted (DOSE and SPIN: the motor has started). |
| `ERR <seq> <code>` | The command was rejected. Nothing moved. `<seq>` is 0 if no seq could be read. |
| `DONE <seq> <steps_done>` | A DOSE or SPIN ended. `<steps_done>` is the exact number of steps made. |
| `FAULT <code> <axis>` | A fault. `<axis>` is `A`, `B`, `C`, `S` (spinner) or `-`. |
| `CFG <seq> key=value ...` | Answer to `CFG` (no separate `OK`). |
| `BOOT <cause>` | Sent once after every reset. `<cause>` is `power`, `ext`, `brownout`, `wdt` or `other`. |

Order for one job: `OK`, then later `DONE`. If a fault ends the job, `FAULT`
comes first, then `DONE` with the steps made so far. The C3 must use
`steps_done` from `DONE` for its dose accounting, never the requested steps.

`DONE` follows every accepted DOSE and SPIN, also after `STOP` or a fault.

### ERR codes

| Code | Reason |
|---|---|
| `bad_format` | Wrong shape: no seq, wrong argument count, stray spaces. |
| `unknown_cmd` | Valid line, unknown command name (names are case sensitive). |
| `bad_arg` | Not a number, unknown axis, zero, or below the minimum speed. |
| `over_limit` | Above a hard limit. The command is rejected, never clamped. |
| `busy` | That motor already moves (one auger at a time). |
| `fault` | A fault is latched. Send `CLEAR` first. |
| `dup` | Same seq as the last accepted DOSE or SPIN. Not run again. |
| `driver` | The TMC2130 does not answer over SPI. |
| `lid_open` | The lid switch is open (only when a lid switch is wired). |
| `not_idle` | `CLEAR` while a motor moves. |

### FAULT codes

All faults except `bad_crc` stop all motors, disable all drivers and latch.
No DOSE or SPIN is accepted until `CLEAR`.

| Code | Reason |
|---|---|
| `stall` | StallGuard tripped on an auger (`<axis>` = A, B or C). |
| `limit` | A hard limit hit while running: the run-time budget ran out, the rate guard fired, or the lid opened. |
| `watchdog` | No valid line for 2 s (see below). `<axis>` is `-`. |
| `driver` | A TMC2130 reports over temperature or a short, or stops answering. |
| `bad_crc` | Informational: a corrupt line arrived. Does not stop motors, does not latch. |

## Hard limits

Compiled in. Values marked PLACEHOLDER in `feeder_limits.h` wait for
calibration. Defaults:

| Limit | Default | Applies to |
|---|---|---|
| `MAX_DOSE_STEPS` | 16000 | DOSE steps |
| `MIN_DOSE_SPEED` / `MAX_DOSE_SPEED` | 100 / 2000 steps/s | DOSE speed |
| `MIN_SPIN_SPEED` / `MAX_SPIN_SPEED` | 100 / 4000 steps/s | SPIN speed |
| `MAX_SPIN_MS` | 5000 | SPIN duration |
| `MAX_RUN_MS` | 8000 | Any command. For DOSE, `steps * 1000 / speed` must not exceed it. |

The step generator interrupt enforces step count and run time again, so a hung
main loop still stops the motor in bounded time.

## Heartbeat watchdog

The first valid line after boot arms the watchdog. After that, if no valid line
(`HB` or any command with a good checksum) arrives for 2 s, the Einsy stops all
motors, disables all drivers and sends `FAULT watchdog -`. The C3 must send `HB`
at least once per second, also when idle. A bad-checksum line does not count.

After a `watchdog` fault, the C3 sends `CLEAR` when it is ready again.

## CFG answer

```
CFG <seq> fw=0.1.0 proto=1 st=idle fault=none drv=1111 ma=400,400 maxst=16000 \
    maxdsp=2000 maxssp=4000 maxsms=5000 maxrun=8000 hb=2000
```

(One line on the wire.) `st` is `idle`, `dose`, `spin`, `dose+spin` or `fault`.
`drv` has four flags (X, Y, Z, E): 1 = the TMC2130 answered with the right chip
version at the last configuration. `ma` is the auger and spinner current in RMS
mA. The `max*` and `hb` fields are the compiled-in limits.

## Hardware watchdog

The AVR watchdog runs with a 1 s timeout and is reset only by the main loop.
A hang resets the board. Heater and fan pins go low at reset, the drivers stay
disabled until a valid command, and the Einsy sends `BOOT wdt`.
