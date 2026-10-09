# Task 9 verification ledger

Status: **NOT STARTED**. This ledger is prepared for the Task 9 branch and
must be populated with fresh command output. Historical Task 6-8 results are
not reused as Task 9 PASS evidence.

## Baseline and safety boundary

- Task 8 merge SHA: record the actual merge commit before beginning Task 9.
- Task 9 branch: `feat/full-integration-hil-validation`.
- Available hardware: ESP32-S3 N16R8 and onboard FAT32 microSD.
- Sensors, MQ-7 heater, and actuators: not connected.
- No flash, reboot, SD format, NVS erase, power-cut, or destructive recovery
  is permitted without separate authorization.

## Evidence matrix

| Area | Required observation | Result | Evidence/limitation |
| --- | --- | --- | --- |
| Firmware archive path | acquisition -> processing -> immutable archive | NOT RUN | Native producer integration required |
| Archive recovery | corrupt tail, fallback, no premature reclaim | NOT RUN | Native fault injection plus target adapter path |
| SD_MMC | mount, bounded I/O, recovery, capacity | BLOCKED | Attached card exists; physical I/O needs explicit authorization |
| ACK lifecycle | exact bytes, valid ACK, conflict/malformed ACK, reclaim | NOT RUN | Use injected ACK fixtures; no synthetic server claim |
| WiFi | association, timeout, backoff, reconnect | NOT RUN | Requires target image and authorized test network |
| HTTPS | staging POST, durable ACK, dedupe, conflict, auth | BLOCKED | Non-production endpoint, CA, device token required |
| BLE | advertising, secure session, scan, candidate credential test | BLOCKED | Browser/device HIL not executed |
| Web UI | gesture, secure context, scan/select/password/retry | BLOCKED | Browser Web Bluetooth environment not available |
| Current projection | late backlog cannot replace newer evidence | NOT RUN | Reuse Task 8 tests only as baseline, rerun on Task 9 integration |
| Schemas/golden hashes | 14 schemas, 24 valid, 36 invalid, frozen hashes | NOT RUN | Must execute on final Task 9 HEAD |
| Resource budget | RAM/flash/stack/heap, queue high-water, latency | NOT RUN | Target build and runtime probes required |
| Sensor/calibration | physical evidence and safety behavior | BLOCKED | No sensors/heater/actuators connected |
| Endurance | 24 h operation and 72 h SD capacity gate | BLOCKED | Physical soak not authorized |

## Required fresh commands

```text
go test ./...
go test -race ./...
go vet ./...
uv tool run --from platformio pio test -e native
uv tool run --from platformio pio check -e native --fail-on-defect=low
uv tool run --from platformio pio run -e sigap_n16r8
uv tool run --from platformio pio run -e sigap_n16r8_sd
uv tool run --from platformio pio run -e sigap_n16r8_sd_spi
uv tool run --from platformio pio run -e sigap_bench
git diff --check
```

Record command, exact commit, toolchain version, duration, and raw result for
each check. Schema output must state the 14/24/36 corpus counts and the frozen
record/body hashes. Firmware output must distinguish target compilation from
physical board execution.

## Hardware evidence rules

Use redacted timestamped logs. A successful build, mock, host filesystem, or
SD card presence is not proof of physical SD I/O, radio coexistence, sensor
accuracy, power-loss recovery, or endurance. If authorization or hardware is
missing, retain `BLOCKED` or `NOT TESTED` and name the exact prerequisite.

## Completion gate

Task 9 requires an independent review after implementation. Do not merge the
Task 9 PR automatically. The final report must include the actual Task 8 merge
SHA, Task 9 HEAD/PR, changed files, evidence matrix, resource measurements,
physical limitations, and unresolved risks.
