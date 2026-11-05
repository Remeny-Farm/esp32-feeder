# Device certificates

The firmware connects to the MQTT broker with mutual TLS. Three files must be
in this directory before `idf.py build`. The build embeds them into the image
(`EMBED_TXTFILES` in `main/CMakeLists.txt`) and fails if one is missing:

| File | Content |
|---|---|
| `ca.crt` | CA certificate. The device trusts only this CA to verify the broker. |
| `device.crt` | Device certificate, signed by that CA. Its CN equals the device id. |
| `device.key` | Private key of the device certificate (PEM, not encrypted). |

## Create them

Run, from the repository root:

```sh
server/certs/make-certs.sh device <id>
```

`<id>` is the device id, for example `feeder-dev`. The script (see `server/`)
produces the CA certificate, a device certificate whose CN is `<id>`, and the
device key. Copy the three files here with the names above.

## Rules

- The certificate CN must equal `CONFIG_FEEDER_DEVICE_ID` (`idf.py menuconfig`,
  Feeder menu). The broker ACL allows the device only the topics
  `feeder/<CN>/...`, so a mismatch means the broker refuses every publish.
- The device id must match `^[a-z0-9][a-z0-9-]{2,31}$`. The firmware refuses to
  start MQTT otherwise.
- Never commit these files. The repository `.gitignore` ignores `*.crt` and
  `*.key`. Do not paste them into issues or chats.
- One key per device. Do not flash the same `device.key` into two units.

## Security note

On a development board the device key is embedded in the flash image and the
flash is not encrypted, so anyone with physical access can read it out. This
is accepted for development. Production hardening (docs/architecture.md):

- Secure Boot v2 and flash encryption (both burn eFuses and cannot be undone).
- Optionally keep the TLS private key in the ESP32-C3 Digital Signature (DS)
  peripheral, so firmware cannot read it at all.
