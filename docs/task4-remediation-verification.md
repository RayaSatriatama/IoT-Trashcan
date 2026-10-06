# Task 4 remediation verification

Date: 2026-10-07
Repository: `RayaSatriatama/IoT-Trashcan`  
Branch: `feat/firmware-maintenance-calibration`  
Starting revision for this bounded remediation: `f1d1efd42682ad49a70c9c6617b39eddc3f50a2b`

This report records the Task 4 maintenance and calibration remediation. It
separates successful upload evidence from native USB maintenance and sensor
hardware-in-the-loop evidence that remains unavailable.

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
- USB profile transfer is bounded and non-repeating. Import chunks are bound to
  the active transfer context, including offset and size checks. Import finish
  only stages a draft; validate, preview, and commit are explicit commands.
  Polling now evaluates idle timeout, transport loss, parser reset, and draft
  cancellation. The real router workflow is covered for both SHT31 addresses.
- DS18B20 evidence now carries independently verified ROM identity, conversion
  completion, and scratchpad CRC state. Calibration maps these fields by name,
  preventing aggregate-initializer drift. Calibration start enters an explicit
  Configuring state bound to the requested target before criteria are supplied.

## Fresh automated verification

| Check | Result |
| --- | --- |
| Native PlatformIO tests | 162/162 passed |
| ASan/UBSan focused native tests | 42/42 passed |
| cppcheck (`--fail-on-defect=low`) | No defects found |
| `sigap_n16r8` | Success, RAM 41,368 B, flash 365,729 B |
| `sigap_n16r8_sd` | Success, RAM 41,368 B, flash 365,729 B |
| `sigap_bench` | Success, RAM 41,368 B, flash 365,729 B |
| `git diff --check` | Clean |

The native suite covers metadata corruption and staged-slot recovery, physical
CALIBRATING evidence, anchor sequencing, provenance hashing, immutable profile
snapshots, DS18B20 identity evidence, profile transfer parsing, complete
HX711 and SHT31 import workflows, idle polling, parser reset, and disconnect
notification.

Task 3 telemetry tests in the firmware native suite continue to pass. The
committed cross-language golden values remain:

- record hash: `sha256:a3a2593618943cc7a1d5f8bc0c90216dc22eb4c3297fb874887c11b7b6ef6883`
- exact body hash: `sha256:3620edf0548b8a15eb1638fc7d239b1995372a28982f7f1e194949160f166c7c`

## Go and schema verification

The official Go 1.27.1 Linux amd64 archive was verified against SHA-256
`63d339f0da5ab53635a56f2490a7984dfe12dfcff22ad749f63edaf590168445` and
installed in the user-owned `/home/raya/.local/opt/go1.27.1` directory. With
that exact executable, `go test ./internal/telemetry/...`, `go test ./...`, and
`go vet ./...` all passed. Semantic schema tests compiled all 14 schemas,
accepted all 24 valid fixtures, and rejected all 36 invalid fixtures with the
expected validator outcomes. The frozen record and exact-body golden hashes
also passed. No repository dependency or module file was changed.

## Hardware inventory and HIL boundary

The connected USB interface was detected as:

- port: `/dev/ttyACM0`
- stable path: `/dev/serial/by-id/usb-1a86_USB_Single_Serial_5A84003000-if00`
- VID:PID: `1a86:55d3`
- USB serial: `5A84003000`

The interface is owned by `root:uucp` with mode `0660`. The user was added to
`uucp`, and a fresh `newgrp uucp` process verified read/write open access. The
corrected `sigap_n16r8` image uploaded successfully through this bridge without
mass erase; esptool identified an ESP32-S3 rev v0.2 with 8 MiB embedded PSRAM,
verified every flash hash, and reset through RTS. No credential operation was
performed.

The official ESP32-S3-DevKitC-1 hardware guide documents two separate USB
connectors: the USB-to-UART port used here for upload, and the ESP32-S3 USB
port used for native USB 1.1/JTAG communication. The firmware reserves
GPIO19/20 for USB D-/D+. No second native USB device appeared in the host
inventory after reset. Reference: Espressif's [ESP32-S3-DevKitC-1 v1.1
hardware guide](https://docs.espressif.com/projects/esp-dev-kits/en/latest/esp32s3/esp32-s3-devkitc-1/user_guide_v1.1.html).

Consequently there are no truthful serial logs or on-device observations for
this run. HIL status:

| HIL case | Status | Reason |
| --- | --- | --- |
| Corrected firmware upload | PASS | External bridge opened in fresh `uucp` process; flash hashes verified |
| Physical maintenance command/response | BLOCKED | Native ESP32 USB Serial/JTAG interface is not visible |
| Config commit and reboot persistence | BLOCKED | Maintenance transport is unavailable on the visible bridge |
| Complete non-repeating profile export | BLOCKED | Native maintenance transport unavailable |
| Import finish leaves active unchanged | BLOCKED | Native maintenance transport unavailable |
| Explicit profile validate/preview/commit | BLOCKED | Native maintenance transport unavailable |
| Connected sensor calibration evidence | BLOCKED | Native maintenance transport unavailable; no approved references supplied |
| CALIBRATING with unrelated channels alive | BLOCKED | Native maintenance transport unavailable |
| Non-heater update preserves acquisition | BLOCKED | Native maintenance transport unavailable |
| MQ7 ADC/HEATER coexistence | NOT TESTED | Sensor/heater setup not established |
| USB inactivity timeout | BLOCKED | Native maintenance transport unavailable |
| USB disconnect cancellation | BLOCKED | Native boundary covered; native USB connection signal unavailable |
| Physical NVS power-cut recovery | NOT TESTED | No controlled power-cut fixture |

The detected device is an external USB-UART bridge used for upload. It is not
the ESP32-S3 native USB maintenance interface. The target adapter reports the
native `Serial` connection state when firmware runs in native USB mode, but no
native USB device appeared after reset and the bridge produced no runtime
maintenance response. Physical disconnect acceptance remains unresolved until
the native USB port is connected and accessible.

Required next physical action: connect a data-capable USB cable to the board
connector labeled `USB` or `ESP32-S3 USB`, not the `USB-to-UART` connector.
Then identify the newly enumerated VID/PID and stable path before opening it.

## Resource audit

Native sizes printed by the acquisition test are:

- `SampleRecord`: 256 B
- `SampleRing<16>`: 4,128 B
- `TargetAcquisitionGraph`: 4,408 B
- `AcquisitionFoundation`: 8,888 B
- profile sizes: HX711 200 B, ultrasonic 208 B, MQ ADC 200 B, MQ7 heater
  208 B, DS18B20 200 B, SHT31 200 B
- `ProfileActivationMetadata`: 96 B in the native representation; the
  serialized NVS envelope is 113 B per copy
- `MaintenanceParser`/`MaintenanceResponse`/`CommandRouter` remain within the
  existing bounded native footprint assertions. The additional SHT31 workflow
  test adds no runtime allocation.

The fixed transfer/parser/response buffers remain bounded. No new unbounded
allocation was introduced. Hardware stack high-water and heap measurements
were not claimed because HIL could not run.

## Scope boundary

Task 5, FIS, Safety, OGI, durable loggers, batch/network workers, HTTPS,
backend archive work, and migration 005 were not started. Profile persistence
maintenance remains within the Task 4 scope; no unrelated backend or frontend
change was made.
