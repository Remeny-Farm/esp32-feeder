# MQTT protocol

JSON Schemas (draft 2020-12) for every message between the backend and a
feeder. The design and the reasons are in [../docs/architecture.md](../docs/architecture.md).

## Topics

`<id>` is the device id. It matches `^[a-z0-9][a-z0-9-]{2,31}$` and equals the
CN of the device certificate and the MQTT client id. The id `backend` is
reserved for the backend service and is not a valid device id.

| Topic | Direction | Schema | Notes |
|---|---|---|---|
| `feeder/<id>/cmd` | backend to device | [cmd.schema.json](schemas/cmd.schema.json) | `scatter`, `cue`, `pause`, `resume`, `set_schedule` (field `type`) |
| `feeder/<id>/ack` | device to backend | [ack.schema.json](schemas/ack.schema.json) | One per command. `reason` only when rejected |
| `feeder/<id>/event` | device to backend | [event.schema.json](schemas/event.schema.json) | `cue`, `scatter`, `fault`; source `random` or `remote` |
| `feeder/<id>/state` | device to backend, retained | [state.schema.json](schemas/state.schema.json) | The Last Will publishes `{"status":"offline"}` |

Shared definitions (timestamps, hopper names, doses) are in
[common.schema.json](schemas/common.schema.json).

## Conventions

- Timestamps are RFC 3339 UTC strings, for example `2026-11-05T10:00:00Z`.
- `at` in `ack`, `event` and online `state` is optional. The device sets it
  whenever its clock is valid. A device without a synced clock omits it, for
  example in a `no_clock` ack, in a fault before the first SNTP sync, or in a
  `state` with `clock_valid: false`. The backend must not rely on `at`.
- Command ids are UUIDs. Every command has `id`, `type`, `issued_at`, `expires_at`.
- Hopper names match `^[a-z][a-z0-9_]{0,15}$`. A device has at most 3 hoppers.
- Grams are integers of 1 or more.
- No message accepts unknown fields (`additionalProperties: false`).
- `set_schedule.limits` can only make the feeder stricter. The firmware still
  checks them against its compiled-in maximums. See "Runtime limits".

## Rules the schemas cannot express

`validate.py` checks these in code. Firmware and backend must enforce them too.

- A hopper appears at most once in `doses` (cmd and event) and in `hoppers` (state).
- `issued_at` is not later than `expires_at`, and `expires_at - issued_at` is
  at most 300 s. The firmware rejects a longer lifetime as `expired`. It also
  rejects a command with `issued_at` more than 5 minutes ahead of its clock.

## Runtime limits

`set_schedule.limits` sets runtime limits. The device combines them with the
compiled-in limits so the result is never looser:

- `max_dose_g`, `daily_budget_g`, `daily_total_g`: the device uses the lower of
  the compiled and the runtime value.
- `min_interval_s`: the device uses the higher of the compiled and the runtime
  value (max). A runtime value can only make the interval longer.
- `quiet_hours`: the quiet time is the union of the compiled and the runtime
  window. A runtime window can only add quiet time. It must not have
  `start` equal to `end`.
- Each `set_schedule` replaces the previous runtime limits. Omitting `limits`
  resets them to the compiled values.

## Delivery rules

- The backend publishes `cmd` with QoS 1 and never retained.
- The MQTT client id equals the device id.
- A command without a readable `id`, or that is not valid JSON, gets no ack. The
  device logs it and drops it. A command with an `id` but otherwise invalid gets
  an `invalid` ack.
- A `cue` command is rate-limited like a scatter: the device rejects it with
  `min_interval` when the last scatter or the last cue-only command is too close.
- `pause` stops random scatters too, not only remote ones.
- The device saves its gate state in NVS: daily counters, last scatter, replay
  ids, pause and faults. A reboot does not reset budgets or the replay check.

## Examples and validation

`examples/valid/` holds messages that must pass. `examples/invalid/` holds
messages that must fail. A file name is `<schema>-<case>.json`, and the prefix
picks the schema. The prefix `deviceid` checks a bare JSON string against the
device id definition in `common.schema.json`.

```sh
python3 -m venv .venv
.venv/bin/pip install jsonschema rfc3339-validator
.venv/bin/python protocol/validate.py
```

The script prints one line per example and exits non-zero if any example
behaves wrongly. Run it after every schema change.
