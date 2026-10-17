# Task 9 commissioning and HIL procedure

## BLE upload gate and automatic NVS write (2026-10-17)

The application-only BLE remediation image was uploaded at `0x10000` and
verified by readback. Image SHA-256:

```text
407dfa560c8bd1dbc07906d5132b88a36af238f3712d7ee65fb01cefb40c9adf
```

The board identity and partition layout matched the approved preflight. The
application reached `mode=RUN`, `SNTP_TIME_READY=PASS`, and
`HTTPS_POLL_ENTERED=PASS`.

Post-boot NVS hashing showed a change. Source review traces it to the normal
first-boot `sigap_ble/setup_secret` creation in
`NvsProvisioningSecretStore::load_or_create()`. This is not a bond deletion,
but it is still an NVS mutation. Under the current authorization,
`NVS_PRESERVED` is therefore `FAIL`, and MITM pairing is paused before local
passkey entry. A scope decision allowing this specific setup-secret write is
required before continuing. Bond deletion remains separately unauthorized.

## BLE MITM pairing remediation gate (`2026-10-17`)

The BLE GATT security contract now requires authenticated permissions on both
directions: `WRITE_AUTHEN | WRITE_NR` for the command characteristic and
`READ_AUTHEN | NOTIFY` for the response characteristic. NimBLE security starts
on a new unauthenticated connection before Web Bluetooth discovery, and the
application still rejects every request unless the connection is encrypted and
authenticated. This prevents an encrypted Just Works link from reaching
identity or Wi-Fi operations.

The current board has a BlueZ bond, but `Paired`, `Bonded`, and `Trusted` are
not evidence of MITM. Do not delete that bond automatically. Before physical
verification, obtain owner authorization for the application-only upload of the
rebuilt image and, only if the bond is confirmed stale and disposable, its
targeted removal. The operator must enter the one-shot passkey locally; the
passkey must not be captured in logs, screenshots, repository files, or chat.

After the upload, verify in Chromium through the real GATT path, in order:

1. Pair with MITM and confirm the local passkey dialog.
2. Read `GET_DEVICE_ID` and compare with `000098a316f5c150`.
3. Read `GET_STATUS` only after identity succeeds.
4. Disconnect and reconnect, then repeat identity verification.
5. Confirm the website clears the verified identity after disconnect.

Until these steps run against the rebuilt image, the physical BLE result is
`NOT_RUN`; synthetic and native tests do not count as physical pairing.

## Runtime-ready application upload (`2026-10-16`)

The approved application-only upload was performed after read-only identity,
partition, and preservation checks. Board identity was ESP32-S3 v0.2,
`98:A3:16:F5:C1:50`, 16 MiB flash, and 8 MiB PSRAM. The installed table
matched `nvs 0x9000/0x5000`, `factory 0x10000/0x400000`, and
`spiffs 0x410000/0x800000`. Secure boot and flash encryption were disabled.

The runtime image was built out of tree with a disposable private certificate
and endpoint. The image SHA-256 is
`4985967c9190ff1a04a67ec4c64306ce30ef11567dd8880e10b6cd5e9550324a`; it is
not a production staging credential package. The exact write was factory-app
only at `0x10000`; no bootloader, partition table, NVS, LittleFS, or SD bytes
were targeted. The factory readback matched the image exactly.

The first image exposed a loop-task stack overflow in empty journal startup.
The source fix removed the large temporary command/result decode objects from
`FsDeviceControlResultJournal::begin()`. The corrected image booted without
the panic and without a LittleFS mount error. Empty journal slots were
observed as missing files, which is the expected empty state for a newly
initialized journal.

Preservation readbacks after upload matched the pre-upload hashes for the
partition table, NVS, and LittleFS. BLE advertisement returned at the expected
address. Authenticated pairing, Wi-Fi association, HTTPS result submission,
and SD access were not performed because this upload authorization did not
include BLE pairing, NVS credential mutation, network credentials, or SD writes.

Required next gates are a separately authorized disposable Wi-Fi/token
configuration and private backend for real HTTPS, plus a separate operator
authorization for authenticated BLE pairing. No scientific calibration or
COMMIT may be attempted without physical sensors and reference instruments.

## Physical LittleFS recovery result (`2026-10-16`)

Owner-approved initialization was limited to the blank `spiffs` partition at
`0x410000` with size `0x800000`. The board was identified read-only as ESP32-S3
revision v0.2, MAC `98:A3:16:F5:C1:50`, 16 MiB flash, and 8 MiB PSRAM. The
installed partition table matched the repository CSV, and flash encryption was
disabled.

The pre-write 8 MiB dump was entirely `0xFF` and matched
`9f9b02f5ee6cbef5e018c1ee424095fc21a842ea6968c0d36114b5930dab2ba1`. The
preservation copy is outside the repository at
`/tmp/sigap-littlefs-backup-before-init-4df9888.bin`. NVS, PHY, partition
table, and factory app hashes were captured before the write and verified again
afterward.

The image was generated offline with PlatformIO `mklittlefs` 2.3 using block
4096, page 256, and an 8 MiB size. Image SHA-256:
`a65d2210306c433023a5be96b22392e03b673ae61471f1676f3f727487c7eade`.
Only `write_flash 0x410000 <image>` was used. Full-chip erase, application
upload, NVS write, partition-table write, profile mutation, WiFi change, and SD
write were not performed.

After a controlled reset, the board booted with matching memory diagnostics and
without the previous LittleFS corrupted-directory or mount-failure messages.
A full post-boot partition readback was accepted by `mklittlefs` with the same
geometry. This establishes physical filesystem mount and integrity, not HTTPS
transport acceptance.

The currently installed application is not HTTPS-enabled, so the production
device-control result journal is not instantiated by its compile-time guard.
Journal save, replay, and corrupted-record recovery therefore remain blocked
until a separately approved HTTPS-enabled application image and disposable
runtime credentials are available. Do not upload an application image as part
of this filesystem-only recovery.

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

## Independent TCP, BLE, and SD follow-up (`2026-10-17`)

The host route and neighbor table resolve the ESP32 through `wlan0`; the Go
listener remains bound to `192.168.0.101:18443`. Kernel TCP drop counters read
without privilege are zero, while the header-only capture still shows ESP32
SYN without SYN-ACK. The host firewall ruleset could not be inspected without
interactive sudo. No policy change was made. A second trusted LAN client was
not available, so host-wide ingress behavior remains unverified.

BlueZ rediscovered the SIGAP service at `98:A3:16:F5:C1:51`, but reports no
pairing, bond, trust, or connection. Authenticated `GET_DEVICE_ID` and
`GET_STATUS` remain pending the one local operator passkey action. The staging
database identity is prepared as canonical `000098a316f5c150`; no passkey was
recorded.

The SD probe was not repeated blindly. Existing output ends during the SD
archive recovery path with no final response. Operation-level timing for the
current image is therefore incomplete and remains a separate diagnostic gate.

## Latest HTTPS runtime blocker (`2026-10-17`)

The current runtime-ready image reaches `DEVICE_CONTROL_READY` and
`HTTPS_POLL_ENTERED` after LittleFS archive recovery. The ESP32 later obtains
`192.168.0.109`; a read-only host check receives 3/3 pings and resolves the
expected board MAC on `wlan0`.

The Go staging listener is bound to `192.168.0.101:18443` and passes host-local
CA verification. A bounded header-only capture records ESP32 SYN packets on
`wlan0`, but no SYN-ACK or RST. No firewall or routing change was made. The
next physical step is an owner-approved host/network policy adjustment or an
approved private endpoint that returns a SYN-ACK to `192.168.0.109`; no TLS or
HTTP acceptance can be claimed before that packet-level prerequisite passes.

Keep SD and BLE checks independent from this network blocker. Do not run
calibration COMMIT, expose credentials, or repeat the archive probe without a
new diagnostic purpose.

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

## Latest authorized application-only run, published at adb0e85

The final out-of-tree `sigap_n16r8_sd_https_staging` image was built with SD_MMC
1-bit, BLE provisioning, native USB maintenance, telemetry, HTTPS device
control, and the LittleFS result journal. The image SHA-256 is
`21446d761fb6cd7952c4230b15c55db5adf5f50a509ec6c7e15da596d9e24d6c`.
Application-only DIO flashing at `0x10000` completed with esptool verification;
no NVS, partition table, LittleFS, or SD write was performed by that operation.

The board booted with matching 16 MiB flash and 8 MiB PSRAM. LittleFS mounted
without the earlier corrupted-directory error. WiFi associated and obtained
`192.168.0.109`. HTTPS remained blocked by host/LAN TCP ingress: host-side TLS
and curl worked, but neither the Go server nor an independent OpenSSL TLS
listener observed a connection from the board. The physical SD probe did not
complete within its bounded 125-second observation window and was not retried
with formatting or destructive recovery.

The next authorized physical steps are narrow and ordered:

1. Allow the board IP to reach the approved private staging host and port, or
   provide an approved private endpoint with matching hostname and CA.
2. In a local unrecorded terminal, obtain and enter the one-shot BLE passkey;
   do not place it in logs, screenshots, browser storage, or repository files.
3. Re-run only read-only BLE `GET_DEVICE_ID` and `GET_STATUS` checks after
   authenticated pairing.
4. Diagnose the SD probe using timing output before any test-owned write.
5. Exercise journal persistence and acknowledged replay only with disposable
   commands and explicit operator approval.

No scientific calibration or COMMIT is authorized by this run. Task 9 remains
HOLD until the physical gates above have direct evidence.
