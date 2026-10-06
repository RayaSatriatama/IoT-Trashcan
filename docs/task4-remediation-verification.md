# Task 4 remediation verification

Date: 2026-10-06  
Repository: `RayaSatriatama/IoT-Trashcan`  
Branch: `feat/firmware-maintenance-calibration`  
Starting revision: `5f3a5331cf28be399b5b7473b317e80d8a7826b5`

This report records the Task 4 maintenance and calibration remediation. It
does not claim sensor calibration accuracy or hardware-in-the-loop validation
where the board could not be flashed.

## Implemented remediation

- Activation metadata now uses one documented, statically checked 113-byte
  envelope with dual committed-generation copies. Recovery never promotes a
  staged slot solely because it has a higher profile version.
- CALIBRATING samples retain physical evidence while suppressing operational
  conversion. Evidence is target-specific, sequence/freshness bounded, and
  ultrasonic echo duration is converted using the active profile rather than
  being treated as millimetres.
- Calibration anchors maintain independent statistics. HX711, ultrasonic,
  DS18B20, and SHT31 profile construction keeps typed criteria and explicit
  references; MQ calibration does not invent ppm, R0, OGI, or thresholds.
- Profile provenance is encoded into hashed bytes, including boot identity,
  monotonic commit time, time quality, reference values, units, and evidence
  summary.
- Acquisition attempts keep immutable profile snapshots. Non-heater changes do
  not reset unrelated acquisition; heater profile changes take effect at a
  safe cycle boundary and can recover from a missing-profile fault.
- USB profile transfer is bounded and non-repeating. Import finish only stages
  a draft; validate, preview, and commit are explicit commands. Polling now
  evaluates idle timeout and disconnect cancellation.

## Fresh automated verification

| Check | Result |
| --- | --- |
| Native PlatformIO tests | 151/151 passed |
| ASan/UBSan focused native tests | 32/32 passed |
| cppcheck (`--fail-on-defect=low`) | No defects found |
| `sigap_n16r8` | Success, RAM 41,368 B, flash 365,209 B |
| `sigap_n16r8_sd` | Success, RAM 41,368 B, flash 365,209 B |
| `sigap_bench` | Success, RAM 41,368 B, flash 365,209 B |
| `git diff --check` | Clean |

The native suite covers metadata corruption and staged-slot recovery, physical
CALIBRATING evidence, anchor sequencing, provenance hashing, immutable profile
snapshots, profile transfer parsing, idle polling, and disconnect notification.

Task 3 telemetry tests in the firmware native suite continue to pass. The
committed cross-language golden values remain:

- record hash: `sha256:a3a2593618943cc7a1d5f8bc0c90216dc22eb4c3297fb874887c11b7b6ef6883`
- exact body hash: `sha256:3620edf0548b8a15eb1638fc7d239b1995372a28982f7f1e194949160f166c7c`

## Go and schema verification

The repository contains the maintained Go JSON Schema validator and the
telemetry conformance tests. The current environment has no `go` executable,
so `go test ./internal/telemetry/...`, `go test ./...`, and `go vet ./...`
could not be executed. Python parsing confirmed all 14 schema files are valid
JSON. The frozen manifest still contains 24 valid and 36 invalid fixtures.
Semantic Go schema execution remains BLOCKED until Go is available.
No dependency was changed to manufacture a pass.

## Hardware inventory and HIL boundary

The connected USB interface was detected as:

- port: `/dev/ttyACM0`
- stable path: `/dev/serial/by-id/usb-1a86_USB_Single_Serial_5A84003000-if00`
- VID:PID: `1a86:55d3`
- USB serial: `5A84003000`

The interface is owned by `root:uucp` with mode `0660`; the current user is
not a member of `uucp`. A non-destructive upload attempt built the corrected
`sigap_n16r8` image successfully but failed before opening the device with
`Permission denied`. No group membership, device permission, mass erase, or
credential operation was performed.

Consequently there are no truthful serial logs or on-device observations for
this run. HIL status:

| HIL case | Status | Reason |
| --- | --- | --- |
| Physical maintenance command/response | BLOCKED | Firmware could not be uploaded/accessed |
| Config commit and reboot persistence | BLOCKED | Same access boundary |
| Complete non-repeating profile export | BLOCKED | Same access boundary |
| Import finish leaves active unchanged | BLOCKED | Same access boundary |
| Explicit profile validate/preview/commit | BLOCKED | Same access boundary |
| Connected sensor calibration evidence | BLOCKED | No serial/runtime access and no known references supplied |
| CALIBRATING with unrelated channels alive | BLOCKED | No serial/runtime access |
| Non-heater update preserves acquisition | BLOCKED | No serial/runtime access |
| MQ7 ADC/HEATER coexistence | NOT TESTED | Sensor/heater setup not established |
| USB inactivity timeout | BLOCKED | No serial/runtime access |
| USB disconnect cancellation | BLOCKED | Native boundary covered; physical signal unavailable |
| Physical NVS power-cut recovery | NOT TESTED | No controlled power-cut fixture |

The Arduino USB transport currently has no reliable DTR/connection signal, so
the native disconnect seam is the available evidence for that boundary.

## Resource audit

Native sizes printed by the acquisition test are:

- `SampleRecord`: 256 B
- `SampleRing<16>`: 4,128 B
- `TargetAcquisitionGraph`: 4,408 B
- `AcquisitionFoundation`: 8,888 B
- profile sizes: HX711 200 B, ultrasonic 208 B, MQ ADC 200 B, MQ7 heater
  208 B, DS18B20 200 B
- `ProfileActivationMetadata`: 96 B in the native representation; the
  serialized NVS envelope is 113 B per copy

The fixed transfer/parser/response buffers remain bounded. No new unbounded
allocation was introduced. Hardware stack high-water and heap measurements
were not claimed because HIL could not run.

## Scope boundary

Task 5, FIS, Safety, OGI, durable loggers, batch/network workers, HTTPS,
backend archive work, and migration 005 were not started. Profile persistence
maintenance remains within the Task 4 scope; no unrelated backend or frontend
change was made.
