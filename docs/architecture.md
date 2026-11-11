# Architecture

Status: draft, 2026-11-05. The concept is in [idea.md](idea.md).

The feeder is a remotely controllable treat feeder with secure endpoints.
It scatters at random times on its own, and it accepts remote scatter
commands. Both paths pass through the same safety gate in the firmware.

## Decisions

| # | Decision | Status |
|---|---|---|
| 1 | MCU: ESP32-C3 SuperMini (already in stock, same board as the farm's BLE receivers) | Accepted |
| 2 | The device opens no inbound port. It connects out to an MQTT broker over TLS with a per-device client certificate (mTLS). | Accepted |
| 3 | Welfare limits live in the firmware. No remote command can exceed them. | Accepted |
| 4 | Three hoppers, each with its own auger and stepper. One shared spinner. | Accepted |
| 5 | Firmware on ESP-IDF (not ESPHome): full control over mTLS and the safety gate | Accepted |
| 6 | First prototype uses salvaged Prusa i3 MK3 parts (Einsy RAMBo as motor controller); the reference design below stays buildable from new parts | Proposed |

Open decisions are listed at the end.

## System overview

```
 App (patron / operator)
        │ HTTPS + user auth
        ▼
 Backend API ── auth, authorization, quotas, audit log
        │ MQTT publish (service certificate)
        ▼
 MQTT broker (TLS 8883, mTLS, per-device topic ACL)
        ▲ outbound connection only
        │
 Feeder (ESP32-C3) ── local scheduler + safety gate
   ├─ sound cue (buzzer)
   ├─ 3 × hopper → auger → stepper (TMC2209)
   ├─ spinner (12 V DC motor)
   └─ sensors: hopper level, stall, motor current, lid
 IP camera ── live view; events carry timestamps to match the stream
```

## Endpoint security

- The device runs no HTTP server and listens on no port.
- Each device has its own client certificate. The broker ACL allows a device
  only its own `feeder/<id>/…` topics.
- Apps never talk to the broker. Every command goes through the backend API,
  which authenticates the user, checks permissions and quotas, and logs the
  request.
- Every command carries a unique `id` and an `expires_at`. The device rejects
  expired commands and command ids it has already seen (replay protection).
- The device rejects a command whose lifetime (`expires_at - issued_at`) is
  longer than 300 s, as `expired`. Each installed replay id keeps its own maximum
  expiry. After 600 seconds of monotonic silence, expired slots may be reclaimed;
  a single far-future id does not pin the other 63 slots. A persisted retired-payload
  expiry cutoff rejects old discarded payloads after a backward clock correction.
- A full 64-id active cache safely pauses and returns `busy` for a new id without
  installing or executing it. That id is not consumed; a later delivery is evaluated
  normally once space exists. Installed live ids are never overwritten, and recovery
  never resumes automatically. All 64 protected future ids can exhaust the finite
  capacity until some are reclaimable; no unbounded rejection history is promised.
- SNTP remains the configured trusted time source. Its native pre-apply hook holds
  an isolated large outlier before it changes the system clock. Two consistent
  replies establish initial time, using 15 s confirmation polling, then normal polling.
  An established good projection remains in use while a single outlier is held.
  Every clock consumer, including state/accounting and random scheduling, sees only
  established time. This filters isolated bad replies, including a bad first reply;
  it does not authenticate two mutually consistent false responses from a malicious source.
- Upgrading version 1 preserves pause, budgets, faults and every cached id. The
  64 unknown-expiry legacy ids move to a separate permanent quarantine, so active
  capacity remains usable. Full printable ASCII ids are encoded losslessly in base95.
- NVS retains an exact 2472-byte v1 `gate` mirror for rollback, a 1928-byte immutable
  `gate_legacy` block and a 384-byte mutable expiry/recovery extension. Signed40
  expiry encoding preserves all RFC3339 dates, including negative epochs; wider
  internal values use a conservative unknown marker. The default partition is unchanged.
  The required write order is legacy, metadata, then mirror, followed by commit.
  These keys are not an atomic transaction. Hash-bound snapshots detect missing or
  interrupted writes, retain known counters/faults and disable admission instead of
  starting empty. No motor begins before the complete save succeeds. Storage-integrity
  disable is not cleared by the service-button fault-clear action.
- The MQTT client id equals the device id, which equals the certificate CN.
  The broker forces the client id to the certificate CN, so a device cannot
  connect under another id (see `server/README.md`).
- Without a clock synced at least once since boot, the device rejects all
  remote commands and the random scheduler does not scatter (quiet hours are
  unknown, fail closed). After the first sync the RTC keeps time, so the random
  scheduler keeps running when the internet drops.
- OTA updates accept only signed images, with automatic rollback on a failed boot.
- Production units enable Secure Boot v2 and flash encryption. These burn
  eFuses and cannot be undone, so development boards keep them off.
- Hardening option: keep the TLS private key in the ESP32-C3 Digital Signature
  (DS) peripheral, so firmware cannot read it out.

## Operating modes

Both modes run the same sequence: **cue → delay → dose → spin → report**.

- **Random.** The scheduler on the device picks random times inside the allowed
  daily window. It works without internet.
- **Remote.** A `scatter` command is a request, not an order. The safety gate
  can reject it, and the device says why in the `ack`.
- **Mix.** One scatter can dose from several hoppers in sequence before the
  spinner runs.

## Safety gate (welfare limits)

The firmware enforces these limits. The backend can lower them, never raise
them above the compiled-in maximums. Values are to be set after a welfare
review. Proposed values: [welfare.md](welfare.md).

- Compiled-in maximums are per-site values. Derive them from the site's flock
  size at build time (see [welfare.md](welfare.md)).
- Maximum dose per scatter, per hopper.
- Daily budget in grams, per hopper and in total.
- Minimum interval between two scatters.
- Quiet hours: no cue and no scatter at night.
- Stall or empty hopper: the motor stops and the device sends a fault event.
- Gate state (daily counters, last scatter, replay ids, pause, faults) is saved
  in NVS, so a reboot does not reset budgets.
- The flock's normal feed and water never depend on the feeder.

## MQTT protocol

| Topic | Direction | Content |
|---|---|---|
| `feeder/<id>/cmd` | backend → device | `scatter`, `cue`, `pause`, `resume`, `set_schedule` |
| `feeder/<id>/ack` | device → backend | accepted or rejected, with a reason |
| `feeder/<id>/event` | device → backend | cue, scatter, dose per hopper, time, source (`random` / `remote`), faults |
| `feeder/<id>/state` | device → backend, retained | mode, hopper levels, daily usage, firmware version; Last Will marks `offline` |

Example command:

```json
{
  "id": "5f0c9a52-7d1e-4b8e-9a43-1f2d3c4b5a69",
  "type": "scatter",
  "doses": [{ "hopper": "mealworm", "grams": 10 }],
  "issued_at": "2026-11-05T10:00:00Z",
  "expires_at": "2026-11-05T10:00:30Z"
}
```

Example rejection:

```json
{
  "id": "5f0c9a52-7d1e-4b8e-9a43-1f2d3c4b5a69",
  "status": "rejected",
  "reason": "daily_budget_exceeded",
  "at": "2026-11-05T10:00:01Z"
}
```

Rejection reasons: `expired`, `duplicate`, `no_clock`, `busy`, `paused`, `quiet_hours`,
`min_interval`, `dose_limit`, `daily_budget_exceeded`, `hopper_empty`, `fault`,
`invalid`. The JSON Schemas are in `protocol/`.

## Hardware

### Mechanics

```
 [Hopper A]    [Hopper B]    [Hopper C]
  grain         pellets       dried mealworms
     │             │             │
  auger+stepper auger+stepper auger+stepper
     └──────┬──────┴──────┬──────┘
            ▼  shared chute
       [spinner, 12 V DC motor]
```

- Each hopper has its own calibration (grams per revolution), because grain,
  pellets and light treats flow differently.
- Feeds never mix in a hopper, and there is no selector mechanism to jam.
- Augers handle dry, free-flowing feed only. Wet or chunky food (vegetables,
  fruit) needs a different mechanism and is out of scope for v1.

### Pin map (ESP32-C3 SuperMini)

Fast signals use native pins. Slow signals go through an MCP23017 I2C expander.
The TMC2209 drivers share one UART bus (addresses 0–2 set by MS1/MS2).

| GPIO | Function | Note |
|---|---|---|
| 0 | STEP, hopper A | |
| 1 | STEP, hopper B | |
| 3 | STEP, hopper C | |
| 4 | I2C SDA | MCP23017 (0x20), INA219 (0x40) |
| 5 | I2C SCL | |
| 6 | UART1 TX → TMC2209 PDN_UART | through 1 kΩ, single-wire bus |
| 7 | UART1 RX ← TMC2209 PDN_UART | |
| 10 | Spinner PWM | logic-level MOSFET |
| 20 | Buzzer PWM | NPN transistor or MOSFET |
| 21 | Spare | prints the boot log at reset; use only for a signal that tolerates it |
| 8 | Status LED (on board) | strapping pin, output only after boot |
| 9 | Service button (on-board BOOT) | strapping pin |
| 18, 19 | USB (flash, console) | reserved |

MCP23017 pins: EN (shared or per driver), DIR A/B/C, hopper low-level sensor
A/B/C, lid switch, with spare pins left.

Limits of this board:

- Three hoppers leave one spare native pin. A fourth hopper uses the last one.
- More than four hoppers needs the TMC2209 internal step generator (VACTUAL)
  instead of STEP pins. Doses are then time-based and less precise.
- The SuperMini PCB antenna is weak. Keep it out of metal enclosures and face it
  toward the access point. A XIAO ESP32-C3 (external antenna) is a drop-in
  fallback.

### Bill of materials (v1)

| Qty | Part | Purpose |
|---|---|---|
| 1 | ESP32-C3 SuperMini | controller |
| 3 | TMC2209 driver module | stepper drivers, UART, StallGuard |
| 3 | NEMA 17 stepper | auger drive |
| 3 | 3D-printed auger + hopper | dosing |
| 1 | 12 V DC gear motor | spinner |
| 1 | Logic-level N-MOSFET + flyback diode | spinner switch |
| 1 | Piezo buzzer or small speaker + transistor | sound cue |
| 1 | MCP23017 module | I/O expander |
| 1 | INA219 module | motor rail current |
| 3 | Low-level sensor (type open) | empty hopper detection |
| 1 | Reed switch + magnet | lid open |
| 1 | 230 V → 12 V power supply, ≥ 2 A | motors (only one stepper runs at a time) |
| 1 | 12 V → 5 V buck converter | board supply |
| 3 | 100 µF capacitor | TMC2209 motor supply |
| 1 | IP65 plastic enclosure | electronics |

The coop has 230 V mains and Wi-Fi coverage.

## Prototype build (salvaged Prusa i3 MK3)

The first prototype uses parts already on hand, so it needs no order. It
proves the augers, the calibration, the spinner and the cue. The reference
design above does not change: other builders can buy its parts new.

```
 ESP32-C3 ── network, mTLS, scheduler, safety gate (unchanged)
   │ UART (commands + heartbeat)
   ▼
 Einsy RAMBo (ATmega2560, 4 × TMC2130)
   ├─ X driver → NEMA 17 (X motor) → auger A
   ├─ Y driver → NEMA 17 (Y motor) → auger B
   ├─ Z driver → NEMA 17 (Z motor, built-in lead screw as shaft) → auger C
   ├─ E driver → NEMA 17 (extruder motor) → spinner
   └─ endstop inputs → hopper level sensors, lid switch
 Power Relay FeatherWing ── cuts the Einsy supply (hardware kill)
```

| Part | Source | Use |
|---|---|---|
| ESP32-C3 SuperMini | stock | controller, as in the reference design |
| Einsy RAMBo | Prusa MK3 | motor controller: 4 silent TMC2130 drivers with StallGuard |
| 4–5 × NEMA 17 | Prusa MK3 (X, Y, Z1, Z2, E) | 3 augers, spinner, 1 spare |
| 24 V power supply | Prusa MK3 | Einsy and motors |
| 12 V power supply | stock | spare; the Einsy also runs on 12 V |
| Bearings, GT2 belts and pulleys, screws | Prusa MK3 | spinner drive, shafts |
| Power Relay FeatherWing (Adafruit 3191) | stock | hardware kill switch on the motor supply |
| i2c 16x2 RGB LCD Pi Plate | stock | optional local service panel |
| 12 V → 5 V buck converter | to source | C3 supply |

Design points:

- **Spinner on a stepper.** The fourth driver runs the spinner, so no DC motor
  is needed. Spinner speed sets the scatter radius, and the firmware controls it.
- **Second safety layer.** The Einsy runs a small custom firmware (not Marlin or
  Prusa firmware, which expect heaters and thermistors). It has its own hard
  limits: maximum steps per command, maximum run time, and a heartbeat watchdog
  that stops all motors when the C3 goes silent.
- **Hardware kill.** The C3 drives the relay. The relay cuts the Einsy supply,
  so a firmware fault on either board cannot keep a motor running.
- **Fewer C3 pins.** The C3 uses UART (GPIO6/7), the relay (GPIO10) and the
  buzzer (GPIO20). I2C (GPIO4/5) stays free for the optional LCD panel.
- **Stall detection.** TMC2130 StallGuard replaces the INA219 current sensor.

To verify before wiring:

- The voltage level of the Einsy UART used for the C3 (the Raspberry Pi header
  of the MK3). The C3 is 3.3 V only.
- The rated current on each motor label, to set the TMC2130 current.
- Whether the Z motor lead screw works as an auger shaft, or the screw must be
  cut and replaced with a printed shaft.

Not used: Feather M0 Bluefruit LE (no Wi-Fi). DC Motor + Stepper FeatherWing
(Adafruit 2927) is a fallback for two motors if the Einsy fails.

## Repository layout

| Path | Content |
|---|---|
| `firmware/` | ESP-IDF firmware |
| `hardware/` | schematic, BOM, STL/CAD |
| `protocol/` | JSON Schemas for the MQTT messages |
| `server/` | minimal reference backend and broker config (Mosquitto with mTLS) |
| `docs/` | idea, architecture, decisions |

App integrations (for example the Remény Farm patron app) live in their own
repositories and use the `protocol/` schemas.

## Open decisions

1. **Broker:** self-hosted Mosquitto or a managed broker (HiveMQ, EMQX, AWS IoT Core)?
2. **Remote control access:** operators only, or patrons too? Patron access
   needs per-user quotas and a queue.
3. **Spinner and chute details:** spinner diameter, height, scatter radius.
4. **Hopper level sensor type:** reflective IR, break beam or time-of-flight.
5. **Wi-Fi provisioning:** BLE provisioning or compiled-in credentials.
6. **Welfare rejection reasons:** per-day scatter count and cue-only limits need
   new rejection reasons in `protocol/` after welfare approval.
