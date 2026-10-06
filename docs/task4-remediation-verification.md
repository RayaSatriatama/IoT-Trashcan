# Task 4 remediation verification

Date: 2026-10-07
Repository: `RayaSatriatama/IoT-Trashcan`  
Branch: `feat/firmware-maintenance-calibration`  
Starting revision for this bounded remediation: `f1d1efd42682ad49a70c9c6617b39eddc3f50a2b`

This report records the Task 4 maintenance and calibration remediation. It
separates automated evidence, native USB maintenance evidence, and sensor
hardware-in-the-loop evidence that still requires physical references.

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

## Hardware inventory and HIL evidence

The earlier upload interface was detected as:

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

The native maintenance interface is now connected and accessible:

- port: `/dev/ttyACM0`
- stable path: `/dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_98:A3:16:F5:C1:50-if00`
- VID:PID: `303a:1001`
- USB serial identity: `98:A3:16:F5:C1:50`
- access: `PASS` from a fresh `uucp` process

The official ESP32-S3-DevKitC-1 hardware guide documents separate USB-to-UART
and ESP32-S3 USB connectors. The firmware reserves GPIO19/20 for native USB
D-/D+. Reference: Espressif's [ESP32-S3-DevKitC-1 v1.1 hardware
guide](https://docs.espressif.com/projects/esp-dev-kits/en/latest/esp32s3/esp32-s3-devkitc-1/user_guide_v1.1.html).

Native `status` returned:

```text
mode=MAINTENANCE
config_version=0
config_hash=0000000000000000000000000000000000000000000000000000000000000000
```

This was the factory draft/bootstrap state. `config draft start` was rejected
because the bootstrap draft already existed. The unchanged factory draft then
passed `config validate`, `config preview`, and `config commit`.
Before reboot, status remained `config_version=0` with the zero hash and
reported `REBOOT_REQUIRED`. The explicit reboot response was received before
reset. After reboot:

```text
mode=RUN
config_version=1
config_hash=8872dc3107056e6ec4f0c1502e078454092fda2c2cf87e1db45d571841d9e540
```

The same configuration identity was returned by `config active`, proving
activation after reboot rather than hot reload.

The boot log also reported:

```text
[SIGAP] board memory flash=16777216 bytes psram_present=true psram=8386295 bytes status=PSRAM_SIZE_MISMATCH
[SIGAP] BOARD_MEMORY_MISMATCH expected_flash=16777216 expected_psram=8388608
```

This is an observed runtime diagnostic and remains a hardware/profile
acceptance gap. No firmware change was made to weaken the identity check.

Native maintenance HIL status:

| HIL case | Status | Reason |
| --- | --- | --- |
| Corrected firmware upload | PASS | Earlier bridge upload and flash hash verification passed |
| Native maintenance status | PASS | `303a:1001`, fresh `uucp` access, response `mode=MAINTENANCE` |
| Config validate/preview/commit | PASS | Factory draft committed without changing runtime before reboot |
| Config persistence after reboot | PASS | Version 1 and nonzero hash reloaded; mode `RUN` |
| Complete non-repeating profile export | PASS | HX711 profile exported in bounded frames: 163 bytes, matching hash `09c724...bb57` |
| Import finish leaves active unchanged | PASS | Import finish staged `DRAFT`; activation required explicit validation/preview/commit |
| Explicit profile validate/preview/commit | PASS | HX711 profile version 2 became `ACTIVE` only after explicit steps |
| Profile persistence after reboot | PASS | HX711 profile version 2 remained `ACTIVE` after controlled reboot |
| Calibration start/cancel before criteria | PASS | `hx711_organic` bound locally; cancel returned `calibration_active=false` |
| USB inactivity timeout | PASS | 305-second idle observation expired the session without another command |
| Physical USB disconnect cancellation | BLOCKED | Host CDC close did not emulate cable removal; physical unplug/reconnect still required |
| Connected sensor calibration evidence | BLOCKED | No documented physical references or sensor evidence supplied |
| CALIBRATING with unrelated channels alive | BLOCKED | Sensor evidence and acquisition observation unavailable through maintenance output |
| Non-heater update preserves acquisition | BLOCKED | No observable connected sensor/acquisition evidence |
| MQ7 ADC/HEATER coexistence | NOT TESTED | Heater wiring and approved timing profile not established |
| Physical NVS power-cut recovery | NOT TESTED | No controlled power-cut fixture |

The QinHeng bridge and native Espressif interface are separate transport paths.
The current firmware maintenance adapter uses native `Serial`; bridge runtime
commands are not evidence for this HIL. Physical disconnect acceptance still
requires unplugging the native USB cable while a calibration or transfer is
active, then reconnecting and verifying the safe state.

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
