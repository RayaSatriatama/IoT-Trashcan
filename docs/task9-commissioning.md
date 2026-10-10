# Task 9 commissioning and HIL procedure

## Current physical storage finding (`2026-10-10`)

The board's LittleFS partition was backed up read-only before any repair
operation. The installed table matches the source layout, but the complete
8 MiB partition image contained only `0xFF` bytes in all sampled blocks
(backup SHA-256:
`9f9b02f5ee6cbef5e018c1ee424095fc21a842ea6968c0d36114b5930dab2ba1`).
Treat the filesystem as unavailable or blank until an owner-approved,
data-preserving recovery is completed. Do not format or erase it. The current
firmware deliberately uses `LittleFS.begin(false)` and keeps HTTPS
device-control disabled when the persistent result journal cannot initialize
or recover. This prevents command execution without durable result replay.

## Current BLE identity gate (`2026-10-10`)

The Wi-Fi maintenance dialog now requires the provisioned canonical
`ble_device_id` and performs authenticated `GET_DEVICE_ID` verification before
`GET_STATUS`, scan, retry, or credential mutation. A failed or disconnected
session clears the verified identity and all cached Wi-Fi state. The BLE
advertising name and service UUID remain display/routing hints only. Physical
MITM pairing and runtime `GET_DEVICE_ID` response verification remain
HUMAN_ACTION_REQUIRED and were not performed in this pass.

Status: **PREPARATION WITH ATTACHED SD PROBE AND BLE DISCOVERY COMPLETE**.
The documented application-only upload and non-destructive SD probe were
executed on the selected historical image. A later scope-override attempt
discovered the attached SIGAP BLE advertisement and opened the real Chromium
device chooser, but no operator selected the device or entered the passkey.
Authenticated pairing, GATT commands, WiFi changes, USB passkey disclosure,
sensor calibration, heater/actuator operation, and destructive tests remain
unverified.

## Preconditions and safety boundary

Before any physical operation, record the exact firmware SHA, board identity,
native USB path, and current maintenance status. The ESP32-S3 N16R8 and its
onboard FAT32 microSD are in scope. Sensors, the MQ-7 heater, and actuators are
not connected; no sensor, calibration, safety, or heater result may be marked
physical PASS.

The external QinHeng USB-UART bridge and the native ESP32-S3 USB JTAG/serial
interface are separate. Native maintenance and disconnect evidence must use
the native interface, not the bridge.

## Secure USB commissioning

1. Start a fresh process/session with access to the `uucp` device group. Use
   the stable `/dev/serial/by-id/` path, verify its target with `readlink -f`,
   and check `fuser` before opening it. Never use `chmod 666`.
2. Capture a redacted `status` response and firmware/build identity. Status
   output must not contain the BLE setup secret, passkey, WiFi password,
   bearer token, or private key.
3. The BLE service retains the existing NVS setup secret and authenticated
   MITM protection. A board without a display requires an owner-approved,
   local-only, one-shot USB passkey handoff before physical pairing. The
   passkey is read directly by the commissioning operator, must not be logged,
   copied into shell history, stored in browser storage, or included in a
   report, and must be invalidated from the output path after one disclosure.
4. Pair only from an HTTPS browser user gesture. Browser permission alone is
   not proof of possession. A failed or repeated handoff must leave the BLE
   service protected and must not rotate the underlying NVS secret.

Expected USB-side mutations are limited to an ephemeral disclosure state and,
if the later owner-authorized flow succeeds, BLE bond/session state. Rollback
is a controlled disconnect and removal of the disposable bond/session using
the documented maintenance path; do not erase NVS.

## Staging HTTPS prerequisites

Use a private, non-production Go process with an isolated SQLite database and a
dedicated disposable device identity/token. Terminate TLS with a reviewed CA
and hostname, and provision the CA and token through the existing SecretStore
boundary. Do not expose the endpoint publicly, commit credentials, or print
request headers. Capture only redacted status and response metadata.

For BLE-enabled records, provision `devices.ble_device_id` with the exact
canonical lower-case 16-hex EFUSE identity returned by authenticated GATT.
Legacy labels such as `SIGAP-01` remain display/archive identifiers and are not
used as BLE identity. A missing mapping keeps BLE mutation unavailable.

The planned transaction evidence is:

1. First exact telemetry-v3 batch bytes produce a durable ACK.
2. An exact replay produces a deduplicated ACK with no duplicate archive.
3. Same batch identity with different body produces conflict and no mutation.
4. Wrong bearer is rejected.
5. Stored exact bytes can be exported and compared without reserialization.

No device-to-staging request has been run in this preparation phase.

## WiFi and BLE checklist

For the current Linux session, `/dev/ttyACM0` is `root:uucp` mode `0660`.
`raya` is already listed in the `uucp` group, but the existing login shell has
not refreshed its supplementary groups. Use a new shell with:

```bash
newgrp uucp
/tmp/sigap-platformio-BRefWz/bin/pio device monitor -p /dev/ttyACM0 -b 115200 --echo
```

Wait for boot output, then enter `maintenance enter` and, only in the local
authorized operator terminal, `ble passkey`. Do not run the passkey command
through a recorded agent tool. Opening the native USB monitor or esptool may
toggle CDC/RTS and reset the board, so record that side effect and do not
repeat it unnecessarily.

Record each item as `PASS`, `FAIL`, `BLOCKED`, or `NOT RUN`, with UTC time and
redacted command/response evidence:

- BLE advertising is visible while WiFi is unconfigured, connecting,
  connected, and retrying.
- Authorized Web Bluetooth pairing succeeds using a user gesture and the
  secure passkey handoff; unsupported browser and insecure-context errors are
  explicit.
- On-demand scan is rate-limited and returns bounded SSID, RSSI, and security
  fields.
- A candidate network is tested before persistence; failure preserves the
  previous working credential.
- Successful WiFi change persists through the existing SecretStore policy,
  reconnects after a bounded failure, and leaves BLE available.
- Disconnect and session timeout clear transient provisioning state without
  exposing credentials.

Required disposable test inputs must be supplied out of band. Do not place
passwords in logs, fixtures, URLs, analytics, or browser local storage.

## SD and archive checklist

The attached-only probe is complete. It mounted without auto-format, used the
device-owned path `/sigap/task9_sd_probe_f516a398.bin`, wrote 256 bytes,
flushed, closed, reopened, verified the exact SHA-256, removed only that path,
and ran archive append/reopen/recovery/contains/reclaim. Result: `sd=PASS`,
`bus=mmc1`, `n=256`, hash
`f987a1b5e0b87fc9e660f09967563a78de60df01dec65236945120eea61d4c56`,
`us=106507021`, total `31252201472`, used `8290304`, free `31243911168`.
The probe image SHA-256 is
`30def8246ef8f34df336f4865f15788ce331b87713db67a0fcb7d45958e1a66a`.
No unrelated file was removed. For future runs, record a before-inventory of
the dedicated test path and available capacity.
Use the existing bounded self-cleaning probe location only, mount with
`format_if_mount_failed=false`, and never format or remove unrelated files.

Measure and retain redacted evidence for:

- mount without auto-format;
- bounded test-file write, flush, close, reopen, exact readback, size, and
  SHA-256;
- archive append, recovery, and exact record readback;
- immutable batch persistence, ACK checkpoint, and reclaim boundary;
- cleanup of only files created by this run and an after-inventory proving
  unrelated data is unchanged.

Host card-reader access is not ESP32 SD evidence. Physical power-loss,
endurance, and 24/72-hour soak tests remain separate acceptance items.

## Memory and timing capture

Capture boot diagnostics without changing firmware configuration: physical
flash/PSRAM capacity, total/free heap, largest free block, and task stack high
water where instrumentation is available. A compiler stack report or a native
fixture is not runtime stack evidence. Record WiFi/BLE/SD coexistence latency,
storage queue high-water, and reconnect duration only from timestamped target
observations.

## Rollback and preservation

- Do not upload unless the owner separately authorizes it and the image SHA,
  NVS-preservation mode, and expected reboot are recorded first.
- Preserve current config/profile identities and export only through approved
  redacted paths.
- Failed candidate WiFi credentials must be discarded, not overwrite the
  previous working credential.
- Staging database and disposable identity are deleted only through the
  explicit staging teardown procedure.
- No physical sensor, heater, actuator, power-cut, or destructive recovery
  evidence can be inferred from these software or attached-only checks.

## Evidence record template

For every case record: case ID, timestamp with timezone, firmware SHA, board
and interface identity, exact bounded action, redacted output, expected result,
actual result, and classification. Keep secrets and raw passkeys outside the
repository. A missing prerequisite is `BLOCKED` or `NOT RUN`, never PASS.
