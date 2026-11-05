# esp32-feeder

An open-source reward-treat feeder for backyard and small-farm chickens.

At random times the feeder plays a short sound cue, then scatters a small
amount of feed. The hens learn the cue and get something to look forward to:
movement and activity, not just a meal. It also accepts remote scatter
commands over secure endpoints. A companion app shows each scatter, and remote
patrons can watch it live.

- ESP32-C3 controller, three hoppers with their own augers, one spinner.
- Outbound-only MQTT over mutual TLS; no open ports on the device.
- Welfare limits enforced in the firmware, for random and remote scatters alike.

> **Status:** design. There is no hardware, firmware or app code yet.
> Concept: [docs/idea.md](docs/idea.md). Architecture: [docs/architecture.md](docs/architecture.md).

## Goals

- Give the flock enrichment: a learned cue, anticipation and foraging.
- Run on its own. Nobody has to trigger each scatter by hand.
- Make each scatter visible to people who are not on the farm.
- Let authorized people trigger a scatter remotely, within safe limits.

## Non-goals

- The feeder is not the main feed source. The flock's normal feeding never
  depends on it.

## Origin

The idea comes from the Remény Farm hen-patronage project (Hungary), where
patrons adopt a real hen and follow her through an app.

## License

- Software (`firmware/`, `server/`, `protocol/`, scripts): [MIT](LICENSE).
- Hardware (`hardware/`: schematics, CAD, STL, BOM): [CERN-OHL-P-2.0](LICENSE-HARDWARE).
- Documentation (`docs/`): MIT.
