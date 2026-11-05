# Reference MQTT broker

A local Mosquitto 2.x broker for development. It follows the contract in
`docs/architecture.md` ("Endpoint security" and "MQTT protocol"). It is a
reference setup, not a production deployment.

## Files

| File | Purpose |
|---|---|
| `mosquitto/mosquitto.conf` | One listener, port 8883, TLS only. No plaintext listener, no anonymous access. Client certificate required. The certificate CN is the MQTT username. |
| `mosquitto/acl` | Topic ACL. Anything not listed is denied. |
| `certs/make-certs.sh` | Creates the CA, the broker certificate, device certificates and the backend certificate. Needs only `openssl`. |
| `certs/out/` | Output of the script. Git ignores everything in it. |
| `data/` | Broker persistence database. Git ignores everything in it. |
| `docker-compose.yml` | Optional: the same broker in `eclipse-mosquitto:2.0.22`, port 8883 only. |

## The contract

- Topics: `feeder/<id>/cmd` (backend to device), `feeder/<id>/ack`,
  `feeder/<id>/event` and `feeder/<id>/state` (device to backend). `state` is
  retained, and the device Last Will goes to `state`.
- Device id: `^[a-z0-9][a-z0-9-]{2,31}$`. The device certificate CN equals the id.
  The firmware sets the MQTT client id to the device id. The broker cannot
  enforce this without the dynamic-security plugin (see "Known limits").
- The backend service has its own certificate with CN `backend`.

The ACL gives a device only its own topics: read `feeder/<id>/cmd`, write
`feeder/<id>/ack`, `/event` and `/state`. The user `backend` may write
`feeder/+/cmd` and read `feeder/+/ack`, `/event` and `/state`. The backend must
subscribe to those three filters one by one. A subscription to `feeder/#` is
denied.

## Why `backend` is a reserved id

The broker takes the username from the certificate CN, and the ACL is keyed on
that name. The rules under `user backend` apply to every client whose CN is
`backend`, whoever issued the certificate. `backend` also matches the device id
pattern. A device certificate with CN `backend` would therefore get the backend
rights: it could command every feeder and read every feeder's data.

The CA must never sign a device certificate with CN `backend`. The script
refuses that id. Anyone who holds the CA key can still sign any CN, so guard
that key. The `pattern` rules also apply to the `backend` user, but they only
expand to `feeder/backend/...`, which no device uses.

## Create certificates

Run from `server/`. Keys are created with mode 600 in `certs/out/`.
The script never overwrites an existing key or certificate unless you pass `--force`.

```sh
certs/make-certs.sh ca
certs/make-certs.sh broker localhost 127.0.0.1    # hostname, then optional IPs
certs/make-certs.sh backend
certs/make-certs.sh device coop-01
```

The hostname must match what the clients connect to. Use `--out DIR` or the
`FEEDER_CERTS_OUT` variable to write somewhere else. Regenerating the CA
(`--force`) invalidates every certificate it signed. For anything beyond local
testing, keep `ca.key` offline and out of the broker host.

## Run

Native (the config uses paths relative to `server/`, so start it there):

```sh
cd server
mosquitto -c mosquitto/mosquitto.conf
```

Docker, with Compose:

```sh
cd server
docker compose up
```

Docker, with a single command (same layout as the Compose file):

```sh
cd server
docker run --rm -p 8883:8883 -w /mosquitto \
  -v "$PWD/mosquitto:/mosquitto/mosquitto:ro" \
  -v "$PWD/certs/out:/mosquitto/certs/out:ro" \
  -v feeder-mosquitto-data:/mosquitto/data \
  eclipse-mosquitto:2.0.22 mosquitto -c mosquitto/mosquitto.conf
```

## Try it

Run from `server/`. Use two terminals.

```sh
# Terminal 1: the device listens for commands.
mosquitto_sub -h localhost -p 8883 --cafile certs/out/ca.crt \
  --cert certs/out/device-coop-01.crt --key certs/out/device-coop-01.key \
  -i coop-01 -t feeder/coop-01/cmd -v

# Terminal 2: the backend sends a command.
mosquitto_pub -h localhost -p 8883 --cafile certs/out/ca.crt \
  --cert certs/out/backend.crt --key certs/out/backend.key \
  -i backend -t feeder/coop-01/cmd -q 1 -m '{"type":"pause"}'
```

Connecting without a client certificate must fail at the TLS handshake.

## Test the ACL

Do not trust the ACL until you have tested the denials. Set these shell
variables first, and create a second device with `certs/make-certs.sh device coop-02`.

```sh
D1="--cafile certs/out/ca.crt --cert certs/out/device-coop-01.crt --key certs/out/device-coop-01.key"
D2="--cafile certs/out/ca.crt --cert certs/out/device-coop-02.crt --key certs/out/device-coop-02.key"
BK="--cafile certs/out/ca.crt --cert certs/out/backend.crt --key certs/out/backend.key"
```

A denied QoS 0 publish is dropped silently, so check the outcome at the other
end. The first check is the most reliable one.

1. **Device cannot write another device's topics.** Terminal 1 runs the
   backend: `mosquitto_sub -h localhost -p 8883 $BK -i backend -t 'feeder/+/event' -v`.
   Terminal 2 runs the next command. Terminal 1 must show nothing.

   ```sh
   mosquitto_pub -h localhost -p 8883 $D1 -i coop-01 -t feeder/coop-02/event -m spoof
   ```

2. **Device cannot write `cmd`, not even its own.** The device `coop-02`
   listens on `feeder/coop-02/cmd` (as in "Try it"). The next command must
   deliver nothing to it.

   ```sh
   mosquitto_pub -h localhost -p 8883 $D1 -i coop-01 -t feeder/coop-02/cmd -m spoof
   ```

3. **Device cannot read another device's commands or use wildcards.** Both
   subscriptions must be refused. With `-V mqttv5 -d` the SUBACK shows
   "Not authorized"; with MQTT 3.1.1 it shows return code 128.

   ```sh
   mosquitto_sub -h localhost -p 8883 $D1 -i coop-01 -V mqttv5 -d -t feeder/coop-02/cmd
   mosquitto_sub -h localhost -p 8883 $D1 -i coop-01 -V mqttv5 -d -t 'feeder/#'
   ```

4. **Device cannot read `state`, `ack` or `$SYS`.**

   ```sh
   mosquitto_sub -h localhost -p 8883 $D1 -i coop-01 -V mqttv5 -d -t feeder/coop-01/state
   mosquitto_sub -h localhost -p 8883 $D1 -i coop-01 -V mqttv5 -d -t '$SYS/#'
   ```

5. **Backend cannot write device topics or subscribe to `feeder/#`.**

   ```sh
   mosquitto_pub -h localhost -p 8883 $BK -i backend -V mqttv5 -q 1 -d -t feeder/coop-01/state -m x
   mosquitto_sub -h localhost -p 8883 $BK -i backend -V mqttv5 -d -t 'feeder/#'
   ```

6. **Positive checks.** These must work: the device reads its own `cmd` and
   publishes its own `ack`, `event` and `state`; the backend publishes `cmd`
   and reads `feeder/+/ack`, `feeder/+/event` and `feeder/+/state`.

Re-run these tests after every change to `mosquitto/acl`.

## Known limits of this setup

- **Client id is not bound to the certificate.** Stock Mosquitto cannot force
  client id = CN. A device with a valid certificate could connect with another
  device's client id and disconnect it. Firmware must use the device id as the
  client id. Binding it at the broker needs the dynamic-security plugin or an
  auth plugin.
- **No revocation.** There is no CRL, and the `pattern` rules cannot deny one
  device. To cut off a lost device, add a `crlfile` (Mosquitto option) with
  that certificate revoked, or rotate the CA.
- **ACL does not control QoS or the retain flag.** The backend must never
  publish `cmd` retained. The protocol's `id` and `expires_at` checks cover
  duplicate delivery.
