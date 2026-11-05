# Feeder firmware

ESP-IDF firmware for the reward-treat feeder (ESP32-C3). It scatters treats at
random times on its own and accepts remote commands over MQTT with mutual TLS.
Every scatter, random or remote, passes the safety gate first
(`components/safety_gate`). Design: [../docs/architecture.md](../docs/architecture.md).

Status: skeleton, app not yet built on hardware. The motor backend is a stub
(it only logs), the lid input is a hook with no hardware behind it, and OTA
download with signature check is not implemented (only the rollback
confirmation is).

## Layout

| Path | Content |
|---|---|
| `main/` | app: Wi-Fi, SNTP clock, MQTT link, protocol JSON, feeder task, random scheduler, NVS |
| `components/safety_gate/` | welfare limits, replay protection (pure C99, host-tested) |
| `components/motor/` | motor API and the stub backend (later: Einsy RAMBo over UART) |
| `components/cue/` | 3-note buzzer cue on GPIO20 (LEDC) |
| `components/feeder_util/` | RFC 3339, HH:MM, random day planner (pure C99, host-tested) |
| `test/host/` | host unit tests, no ESP-IDF needed |
| `certs/` | device certificates (gitignored, see `certs/README.md`) |

## Build

Prerequisites: ESP-IDF v5.3 or newer, activated in the shell (`. $IDF_PATH/export.sh`).

```sh
cd firmware
# 1. Put ca.crt, device.crt, device.key into certs/ (see certs/README.md).
idf.py set-target esp32c3
idf.py menuconfig        # Feeder menu: device id, Wi-Fi, broker, hoppers, limits
idf.py build flash monitor
```

The device id must equal the CN of `certs/device.crt`.

## Host tests

```sh
cmake -S firmware/test/host -B build-host && cmake --build build-host && ctest --test-dir build-host --output-on-failure
```

## Welfare

Every limit in the Feeder menu (max dose, daily budget, minimum interval, quiet
hours, random dose) is a PLACEHOLDER. Set the real values after a welfare
review before any hen eats from this device. Remote commands can only lower
the limits, never raise them above the compiled-in values.
