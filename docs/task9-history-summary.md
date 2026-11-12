# SIGAP Task 9 history summary

This document summarizes the SIGAP Task 9 work from the repository foundation
through the current source head. It is an index and explanation of the
existing ledgers, not a replacement for their timestamped evidence. A result
marked `software` or `synthetic` is never a physical-device result.

## Current state

| Item | Value |
| --- | --- |
| Repository | `RayaSatriatama/IoT-Trashcan` |
| Branch | `feat/full-integration-hil-validation` |
| Current HEAD | `b0de424f0f870ea695ca721ac81a28c124762d61` |
| Current commit | `fix(ble): avoid BlueZ UUID discovery filter crash` |
| Worktree | Clean after the current documentation commit is pushed |
| Pull request | #27, OPEN and unmerged |
| Task 9 | HOLD |
| Task 10 | NOT STARTED |
| Physical sensors | Not connected |
| Physical calibration | Excluded from completed evidence |

The current HEAD contains the Web Bluetooth discovery mitigation and its
regressions. The latest firmware source evidence before this frontend-only
change was already green. The current source does not claim physical BLE
pairing, device identity readback, HTTPS transaction completion, or sensor
calibration.

## Scope and safety boundary

Task 9 was executed under a strict separation between software evidence and
hardware evidence.

Allowed work included source changes, native firmware tests, disposable
SQLite and TLS staging, synthetic evidence, browser software tests,
documentation, read-only hardware inspection, and explicitly authorized
non-destructive device operations.

The following were not treated as completed unless direct evidence existed:

- BLE MITM pairing and authenticated GATT commands.
- ESP32 Wi-Fi provisioning or credential mutation.
- ESP32-originated HTTPS and telemetry ACK.
- Physical sensor evidence or scientific calibration.
- Heater and actuator operation.
- Physical power-loss, cable-removal, or destructive recovery tests.
- 24-hour and 72-hour endurance runs.
- PR merge or Task 10 work.

No password, BLE passkey, bearer token, private key, or physical sensor value
was added to the repository or the evidence documents.

## Architecture delivered

The final software design has one authoritative firmware calibration path:

```text
Local Web Bluetooth
  -> authenticated GATT adapter
  -> CalibrationTransportController
  -> CalibrationSession and CalibrationEvidenceCache
  -> SET_CRITERIA / CAPTURE / VALIDATE / PREVIEW / COMMIT
  -> ProfileManager and durable profile readback
  -> browser

Remote HTTPS
  -> authenticated operator session
  -> Go HTTP API and SQLite device-control queue
  -> authenticated outbound HTTPS poller
  -> same firmware controller, session, evidence, and ProfileManager
  -> durable result reconciliation
  -> browser
```

USB maintenance remains a bounded recovery and commissioning adapter. It does
not introduce a second calibration engine.

Firmware remains authoritative for acquisition evidence, quality and validity,
calibration coefficients, physical conversions, profile activation and
persistence, FIS and safety behavior, and telemetry-v3 records. Browser and
Go layers carry typed commands and results but never fabricate measurements,
MQ ppm/R0, gas indicators, coefficients, or device evidence.

## Chronology

### Repository and platform foundation

The repository began with the SIGAP monitoring application and then acquired
the firmware, telemetry, storage, safety, and backend foundations.

| Period | Key commits and result |
| --- | --- |
| 2026-09-16 to 2026-10-01 | Repository initialization, authenticated ingestion, monitoring UI, simulator isolation, firmware configuration, acquisition foundation, telemetry-v3 contract, and PRD/TSD specifications. |
| 2026-10-07 | Maintenance calibration and FIS/safety foundations were integrated. |
| 2026-10-08 | Wi-Fi/HTTPS transport foundation and durable archive/immutable batching were added. |
| 2026-10-08 | Backend telemetry-v3 integration completed. This established the base for Task 9. |

Important foundation commits include `3e93580`, `3c59520`, `706117c`,
`c07f512`, `5a37365`, `aba6555`, `57d0749`, `67416b7`, and `6269b6c`.

### Task 9 implementation wave

The initial Task 9 plan defined WEB-CAL-01 through WEB-CAL-18 and assigned
five implementation areas: shared firmware controller, BLE adapter, Go
queue/API, website diagnostics and wizard, and lead integration.

The implementation added:

- Bounded, versioned calibration command and result envelopes.
- One session owner across USB, BLE, and HTTPS.
- Typed criteria, references, capture anchors, evidence, preview, commit, and
  cancel operations.
- Sensor-specific capabilities for all 13 channels.
- Go SQLite command queue, lease, expiry, ownership, result reconciliation,
  replay, and conflict handling.
- Telemetry-v3-backed diagnostics that do not use arbitrary command results as
  sensor evidence.
- Website wizard stages for diagnostics, START, criteria, capture, validate,
  preview, commit, readback, and completion.
- Secure-context and user-gesture Web Bluetooth boundaries.
- In-memory browser operator credentials with no localStorage or
  sessionStorage persistence.

Key implementation commits were `5cb14c1`, `7c3af3f`, `66ec45d`, `75eb2da`,
`0fe068a`, `328e140`, `4af6e27`, `23e92f4`, `ca55c11`, `ea8aea8`, `38680b7`,
`e5e7d20`, `1c9a5f2`, `4fc5bdd`, `bf37efc`, and `8995590`.

### Cross-layer contract remediation

Independent review found integration defects despite passing isolated tests.
The remediation closed these gaps:

1. Operator status responses were separated from firmware delivery envelopes.
   Browser polling now reads durable command status, result, error, expiry, and
   lifecycle timestamps from SQLite.
2. Diagnostics now use a chronology-aware telemetry-v3 projection. A PREVIEW,
   START, or COMMIT response cannot masquerade as live sensor evidence.
3. BLE capabilities include the selected sensor ID. START carries validated
   sensor-specific references and engineering parameters.
4. Wizard state separates operator step, session state, and command status.
   Failed or merely accepted delivery cannot skip criteria or capture.
5. BLE COMMIT carries the exact validated preview version and hash.
6. HTTPS command parsing validates nested fields, types, request identity,
   expiry, sensor identity, and bounded payloads.
7. Telemetry-v3 current and archive projections remain authoritative for the
   website; legacy research views cannot be used as raw hardware evidence.

The principal review and integration commits were `b48eac1`, `75f9634`,
`a2f9d35`, `83f22a3`, `e01a81e`, `76c3203`, `0a11d1c`, `c745c7d`, and
`d5ce1a9`.

### P0 durability and identity remediation

Two production P0 gaps were identified and fixed in source:

- `FsDeviceControlResultJournal` is bound into the production
  `DeviceControlPoller` composition. Journal state is bounded, versioned,
  integrity checked, and fail-closed for corrupt or unavailable storage.
- BLE identity is bound to the canonical provisioned `ble_device_id`.
  Advertised name, address suffix, and service UUID are discovery hints only.
  Authenticated `GET_DEVICE_ID` must match before status, scan, retry, Wi-Fi,
  or calibration mutations are allowed.

Durable COMMIT recovery now follows this order:

1. Persist a prepared intent before profile activation.
2. Activate only after expected profile identity validation.
3. Persist the exact terminal result and profile version/hash.
4. Retry immutable result bytes after a lost HTTP response.
5. Reconcile against active profile identity without activating twice.
6. Clear the journal only after backend acceptance.
7. Reject a different unreconciled prepared identity and fail closed on
   corrupted state.

Replay authorization runs before replay lookup. USB, BLE, and HTTPS cannot
   concurrently mutate one session. These fixes were covered by native fault
   injection, controller recreation, persistence failure, replay conflict, and
   transport contention tests.

Key commits were `d5d0169`, `a7e9163`, `008a9c3`, `cf1e023`, `494e360`,
`ed3bec1`, `14a27fd`, `cd9acc8`, `6b2028a`, and `3051eef`.

### Physical LittleFS recovery

The first physical board inspection found:

- ESP32-S3 revision v0.2.
- Flash 16 MiB.
- PSRAM 8 MiB.
- USB identity `98:A3:16:F5:C1:50`.
- BLE address `98:A3:16:F5:C1:51`.
- `spiffs` partition at `0x410000`, size `0x800000`.
- LittleFS error `Corrupted dir pair at {0x0, 0x1}` and mount error `-84`.

A read-only 8 MiB dump was all `0xFF` and matched the preserved hash
`9f9b02f5ee6cbef5e018c1ee424095fc21a842ea6968c0d36114b5930dab2ba1`.
The partition table, NVS, PHY data, factory application, and SD card were
preserved. Owner authorization then allowed initialization of only the
LittleFS partition.

An offline 8 MiB LittleFS image was generated with `mklittlefs` 2.3, block
4096, page 256. Image hash:
`a65d2210306c433023a5be96b22392e03b673ae61471f1676f3f727487c7eade`.
Only `0x410000..0xC10000` was written. Post-write readback and boot evidence
confirmed a mountable filesystem without formatting or rewriting other
partitions.

Physical status at that stage:

```text
LITTLEFS_BACKUP=PASS
PARTITION_LAYOUT=PASS
LITTLEFS_IMAGE=PASS
LITTLEFS_WRITE=PASS
LITTLEFS_MOUNT=PASS
LITTLEFS_INTEGRITY=PASS
NVS_PRESERVATION=PASS for that scoped operation
SD_PRESERVATION=PASS
DEVICE_CONTROL_JOURNAL=BLOCKED because the installed image was non-HTTPS
```

### Runtime SD and HTTPS image work

The first runtime SD+HTTPS image exposed a loop-task stack overflow while an
empty result journal was initialized. The cause was two large temporary
command/result objects in `FsDeviceControlResultJournal::begin()`. The fix
moved decoding to bounded instance-owned buffers and preserved fail-closed
startup. The corrected image was byte-verified after application-only upload.

Later physical runtime diagnostics exposed an archive recovery stall. The
archive had 26,364 records and a 6,291,396-byte payload. Prefix duplicate
lookups were being repeated across boot boundaries even though boot ID already
formed part of immutable identity. The recovery path was changed to skip that
lookup for a new boot and retain it only for same-boot out-of-order records.
Physical recovery then completed in approximately 8.8 seconds and reached the
HTTPS poller.

Runtime gates reached PASS in that run:

```text
CONFIG_RUNTIME_READY
LITTLEFS_MOUNTED
FILESYSTEM_RECOVERY
JOURNAL_READY
NETWORK_SECRETS_LOADED
NETWORK_READY
DEVICE_CONTROL_READY
SETUP_COMPLETE
LOOP_ALIVE
SNTP_TIME_READY
HTTPS_POLL_ENTERED
```

The later firmware boot also created the normal first-boot
`sigap_ble/setup_secret`. The NVS hash changed as a result. No agent command
erased NVS or deleted a bond, but `NVS_PRESERVED` is now recorded as FAIL for
that image. Additional physical pairing or provisioning mutation is paused
behind the NVS authorization gate.

### SD evidence and recovery limit

An earlier attached SD probe passed on SD_MMC 1-bit and created then removed
only the device-owned test path. Recorded evidence included exact readback,
SHA-256 validation, archive append/reopen/recovery, ACK checkpoint, reclaim,
and a probe duration of `106507021` microseconds.

The newer runtime image exposed a separate archive recovery stall, later fixed
in source. Current physical SD acceptance is not claimed as a final clean
result because the full post-fix operation was not completed in the latest
physical run. No format, erase, or unrelated SD write was performed.

### HTTPS diagnosis

The device associated with Wi-Fi and received `192.168.0.109`. The approved
private Go listener was `192.168.0.101:18443`. Host-local CA-verified curl
worked. Read-only packet capture observed ESP32 SYN packets, but no SYN-ACK,
RST, or established TCP connection.

```text
TCP_SYN_OBSERVED=PASS
TCP_SYN_ACK_OBSERVED=FAIL
TCP_ESTABLISHED=FAIL
TLS_HANDSHAKE=NOT_RUN
GO_REQUEST_RECEIVED=NOT_RUN
TELEMETRY_ACK=NOT_RUN
FIRST_REAL_E2E=BLOCKED_BY_HOST_TCP_RESPONSE
```

No firewall, route, AP policy, or public exposure change was made. A permitted
private endpoint or an owner-approved host ingress correction is required
before physical HTTPS, telemetry ACK, and result-journal durability can be
accepted.

### BLE physical and host diagnosis

The board advertises as `SIGAP-A398` and exposes the SIGAP service UUID
`7d3f0001-7d3f-4a53-9a4b-534947415000`. BlueZ discovery succeeded, but the
host daemon repeatedly crashed with SIGSEGV during the earlier UUID-filter
flow. BlueZ 5.87 package integrity was clean; the coredump payload was not
available for symbolic analysis.

The firmware BLE security remediation changed GATT permissions to require
authenticated access and starts NimBLE security on a new unauthenticated
connection. Application `isAuthenticated()` and identity checks remain
mandatory. Passkeys were never logged and bonds were not deleted.

The current frontend mitigation avoids the BlueZ 5.87 UUID discovery-filter
path. `requestDevice()` now uses:

```ts
{
  filters: [{ namePrefix: 'SIGAP-' }],
  optionalServices: [SIGAP_BLE_SERVICE_UUID],
}
```

The service UUID is still used after connection for GATT access. The name is
not treated as authentication. Chromium loaded the rebuilt bundle, opened the
real chooser, and no new `bluetoothd` crash/restart occurred during the bounded
observation. The chooser was cancelled before selection.

Current physical BLE status:

```text
REAL_BLE_DISCOVERY=PASS
UUID_FILTER_REMOVED=PASS
NAME_FILTER_DISCOVERY=PASS
BLUETOOTHD_STABLE_AFTER=PASS (bounded observation only)
REAL_BLE_PAIRING=NOT_RUN
BLE_MITM_AUTHENTICATED=NOT_RUN
REAL_GET_DEVICE_ID=NOT_RUN
REAL_GET_STATUS=NOT_RUN
BLE_RECONNECT=NOT_RUN
```

The remaining human action is local operator selection of `SIGAP-A398` and
passkey entry after the NVS gate is resolved. The passkey must stay local.

## Software changes by area

### Firmware

- Shared calibration transport controller and session ownership.
- Typed BLE F1/R1 and HTTPS command/result codecs.
- Strict bounds, expiry, request hash, identity, and expected profile checks.
- Durable COMMIT intent and terminal-result recovery.
- Persistent LittleFS device-control result journal.
- Sensor-specific typed criteria and reference handling for 13 channels.
- Fail-closed network, journal, profile, and evidence paths.
- Archive recovery optimization and loop-task stack safety fix.
- SD_MMC and SD_MMC SPI target configurations.
- Runtime diagnostics for setup, filesystem, journal, network, SNTP, and poller
  transitions.

### Go backend

- SQLite command queue with durable status and result readback.
- Device and operator authorization, CSRF/origin, expiry, lease, and replay
  controls.
- Late result reconciliation after lost device responses.
- Exact result bytes and profile identity preservation.
- Telemetry-v3 ingest, ACK, archive, cursor, and export paths.
- Disposable TLS/SQLite loopback staging.

### Website and BLE client

- Diagnostics panel backed by authoritative telemetry-v3 data.
- Wizard state machine with explicit lifecycle stages.
- Sensor-specific capabilities and parameter delivery.
- Exact preview-to-COMMIT identity binding.
- Authenticated BLE identity gate before status or Wi-Fi mutation.
- No credentials in browser persistent storage.
- BlueZ UUID-filter mitigation using `namePrefix` and `optionalServices`.

### Documentation and operations

- Task 9 verification ledger and commissioning runbook.
- Loopback HTTPS staging procedure and secret boundary.
- Sensor commissioning requirements and physical exclusion list.
- Dependency audit and bounded remediation rationale.
- This historical summary.

## Fresh and historical verification

### Current HEAD `b0de424`

| Check | Result |
| --- | --- |
| Focused BLE client and Task 9 integration tests | PASS, 16/16 |
| Frontend full Vitest suite | PASS, 22 files and 119 tests |
| TypeScript typecheck | PASS |
| Expo web export | PASS |
| `git diff --check` | PASS |
| Rendered Chromium bundle | PASS, patched discovery strings present |
| Real chooser invocation | PASS, chooser opened; cancelled before pairing |
| New BlueZ crash during bounded observation | None observed |

### Latest integrated firmware source evidence before the frontend-only patch

The current ledger records the following source evidence from the integrated
firmware and backend remediation:

| Check | Recorded result |
| --- | --- |
| PlatformIO native suite | PASS, 455/455 |
| ASan/UBSan native suite | PASS in the preceding integrated runs, with 430/430 and 450/450 snapshots preserved historically |
| Cppcheck | PASS, no defects |
| Standard firmware targets | PASS |
| HTTPS composition targets | PASS compile-only |
| Go tests | PASS |
| Go race tests | PASS |
| `go vet ./...` | PASS |
| Telemetry schemas and frozen hashes | PASS |
| Frontend before current discovery patch | PASS, 118/118 |

The six target measurements recorded at the most recent integrated source
stage were:

| Environment | RAM | Flash |
| --- | ---: | ---: |
| `sigap_n16r8` | 49,496 | 1,111,181 |
| `sigap_n16r8_sd` | 50,036 | 1,184,445 |
| `sigap_n16r8_sd_spi` | 49,792 | 1,163,857 |
| `sigap_bench` | 49,496 | 1,111,181 |
| `sigap_n16r8_https_compile` | 51,860 | 1,313,213 |
| `sigap_n16r8_sd_https_compile` | 52,300 | 1,371,521 |

These are source/build evidence. They do not prove runtime HTTPS, BLE pairing,
physical SD integrity, sensor validity, or power-loss behavior.

Frozen telemetry hashes remain unchanged:

```text
record sha256:a3a2593618943cc7a1d5f8bc0c90216dc22eb4c3297fb874887c11b7b6ef6883
body   sha256:3620edf0548b8a15eb1638fc7d239b1995372a28982f7f1e194949160f166c7c
```

## Acceptance matrix

### AC01 through AC35

The matrix below separates software evidence from physical or elapsed-time
evidence. Definitions follow the PRD/TSD acceptance table.

| ID | Software/native evidence | Physical or long-run status |
| --- | --- | --- |
| AC01 | PASS, clean builds and native tests | Board identity/build upload evidence exists, final physical source image not re-uploaded after `b0de424` |
| AC02 | PASS, N16R8 target configuration and memory checks | PASS for observed board memory; full pin acceptance separate |
| AC03 | PASS synthetic acquisition path | BLOCKED, sensors absent |
| AC04 | PASS typed calibration path | BLOCKED, no physical fill references |
| AC05 | PASS validation and profile tests | BLOCKED, no reference-load run |
| AC06 | PASS safety and negative tests | EXCLUDED, MQ-7 heater not activated |
| AC07 | PASS fault-injection and disconnected evidence tests | Physical sensor faults NOT RUN |
| AC08 | PASS Go/API compatibility tests | No physical ingest |
| AC09 | PASS exact replay and conflict tests | Physical replay NOT RUN |
| AC10 | PASS software archive tests; prior bounded SD probe recorded | Final post-fix physical archive run INCOMPLETE |
| AC11 | PASS software interruption tests | Physical power-loss NOT RUN |
| AC12 | PASS clock and monotonic-expiry tests; SNTP runtime reached PASS | Full cold-boot time campaign NOT RUN |
| AC13 | PASS telemetry-v3/export fixtures | No physical export source |
| AC14 | PASS redaction and source-boundary tests | No production credential deployment |
| AC15 | PASS bounded resource tests | 24-hour soak NOT RUN |
| AC16 | PASS unavailable/negative scientific model tests | Physical scientific validation BLOCKED |
| AC17 | PASS software draft/preview/commit/cancel | Physical calibration NOT RUN |
| AC18 | PASS crash-consistency and replay tests | Physical power-cut validation NOT RUN |
| AC19 | PASS storage fault tests | 72-hour SD endurance NOT RUN |
| AC20 | PASS fallback and pending-preservation tests | Physical SD fault/removal NOT RUN |
| AC21 | PASS calibration-state contract tests | No physical calibration |
| AC22 | PASS hardware-v3 backend boundary tests | No live hardware stream |
| AC23 | PASS atomic/idempotent Go transaction tests | Physical transaction NOT RUN |
| AC24 | PASS provenance and cross-language fixtures | Physical provenance NOT RUN |
| AC25 | PASS stable record/hash reconciliation tests | No physical archive campaign |
| AC26 | PASS quota and no-overwrite tests | Physical quota exhaustion NOT RUN |
| AC27 | PASS UTC-null and derived-time tests | Physical clock campaign NOT RUN |
| AC28 | PASS cursor/backlog tests | Physical backlog drain NOT RUN |
| AC29 | PASS bounded queue tests | Long-run backlog drain NOT RUN |
| AC30 | PASS static architecture checks | No separate physical gate |
| AC31 | PASS native NVS/config interruption tests | Current board had an automatic setup-secret NVS write; preservation gate FAIL for latest image |
| AC32 | PASS corrupt/invalid config tests | Physical corruption test NOT RUN |
| AC33 | PASS config identity and boot tests | Runtime boot evidence exists, final source image not re-uploaded |
| AC34 | PASS secret separation/redaction tests | Production provisioning BLOCKED |
| AC35 | PASS profile/reference schema tests | Physical profile activation with sensors BLOCKED |

### WEB-CAL-01 through WEB-CAL-18

The PRD/TSD defines these IDs as a grouped acceptance family rather than
separate prose descriptions for each numeric ID. The repository mapping is:

| Acceptance group | Software result | Physical limitation |
| --- | --- | --- |
| WEB-CAL-01..09: shared controller, 13 channels, local/remote workflow, typed evidence and profile lifecycle | PASS in native, Go, frontend, and synthetic integration tests | Physical references, BLE, Wi-Fi, and sensor evidence BLOCKED |
| WEB-CAL-10..12: authenticated sessions, device binding, expiry, idempotency, and transport ownership | PASS in Go/device-control and BLE contract tests | Physical authenticated GATT and operator-provider deployment BLOCKED |
| WEB-CAL-13..14: profile provenance, exact identity, telemetry-v3 diagnostics and readback | PASS in API, codec, replay, and fixture tests | Live device/backend readback NOT RUN |
| WEB-CAL-15..18: offline safety, USB recovery, deterministic synthetic tests, and unsupported operations | PASS in native/frontend tests and explicit unsupported responses | Physical USB/HIL and sensor validation NOT RUN |

## Remaining blockers and next actions

1. Host Bluetooth stability improved after replacing UUID discovery filtering,
   but authenticated pairing and GATT readback still require a local operator
   action. No bond deletion is authorized.
2. The latest NVS preservation gate is FAIL because the firmware created the
   first-boot BLE setup secret. A scope decision is required before any new
   physical provisioning mutation.
3. ESP32 SYN packets reach the host, but the private HTTPS listener does not
   return SYN-ACK. A permitted private endpoint or owner-approved TCP ingress
   correction is required. Firewall changes were not performed.
4. Physical SD acceptance after the archive recovery fix needs one bounded,
   test-owned run with operation-level timings. No formatting or unrelated
   writes are allowed.
5. Result-journal physical durability requires an actual device-originated
   HTTPS request, backend acceptance, restart/replay, and acknowledged clear.
6. Sensor calibration, MQ-7 heater, actuators, scientific coefficients, and
   physical power-loss/endurance gates remain excluded or deferred.

## Evidence and source index

- `docs/task9-verification.md`: timestamped verification ledger and raw status.
- `docs/task9-commissioning.md`: physical preflight, upload, BLE, Wi-Fi, SD,
  and rollback procedures.
- `docs/task9-staging.md`: disposable Go/SQLite/TLS staging procedure.
- `docs/task9-web-calibration.md`: transport, commit, wizard, and sensor
  reference contract.
- `docs/task9-sensor-commissioning.md`: physical reference and sensor safety
  requirements.
- `tasks/plan-task9-web-calibration.md`: implementation decomposition.
- `tasks/todo-task9-web-calibration.md`: checklist and remaining provider gate.
- `docs/Microcontroller/PRD_Firmware_SIGAP_ESP32S3_PlatformIO.md`: AC01-AC35
  and WEB-CAL authority.
- `docs/Microcontroller/TSD_SIGAP_ESP32S3_PlatformIO.md`: telemetry, storage,
  transport, and verification contract.

## Final verdict

```text
TASK_9_SOFTWARE=COMPLETE for the implemented and verified software scope
TASK_9_BROWSER_SOFTWARE=PASS for synthetic and rendered chooser invocation
TASK_9_REAL_BLE=INCOMPLETE, pairing and authenticated GATT not verified
TASK_9_REAL_HTTPS=BLOCKED_BY_HOST_TCP_RESPONSE
TASK_9_REAL_SD=INCOMPLETE after the post-fix physical run gap
TASK_9_SENSOR_CALIBRATION=EXCLUDED_BY_HARDWARE
TASK_9_OVERALL=HOLD
PR_27=OPEN_UNMERGED
TASK_10=NOT_STARTED
```

The software implementation is documented as complete only for the source,
tests, contracts, and synthetic integration that have direct evidence. The
overall Task 9 gate remains HOLD until the separately controlled physical and
deployment requirements are resolved.
