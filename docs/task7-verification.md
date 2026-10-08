# Task 7 network transport verification

## Scope

Task 6 was merged into `main` at `57d0749632f29b18fd1086b893868a5612b7c935`.
This branch adds the bounded Wi-Fi/HTTPS archive-v3 transport foundation. It
does not add BLE, the legacy summary worker, backend archive code, migration
005, or actuator output.

Final verified source SHA for this report: `652cd11e99296850f7cbfb5c04c2cb48cb1e806d`.

## Runtime behavior

- `NetworkWorker` consumes the immutable `ArchiveBatchRuntime` view after
  storage and batch polling. It never reads the acquisition handoff.
- The request body is `ImmutableBatch::exact_body()` and is never rebuilt for
  retries. The durable batch body hash is checked before every send.
- Wi-Fi loss, timeout, 429, 5xx, malformed ACK, and ACK mismatch retain the
  pending batch. Retry delay is bounded by the policy and `Retry-After` ceiling;
  protocol faults also receive a bounded delay to avoid a loop-rate spin.
- A 200 response advances the local delivery journal only after device ID,
  batch ID, body hash, record count, and stored-plus-deduplicated counts match.
- 401/403 enters `AuthRequired`; 409 and other protocol errors remain pending
  and are observable as `Conflict`/`ProtocolError`. No local reclaim occurs.
- Target HTTPS is opt-in through an explicit endpoint and CA build boundary.
  No endpoint, certificate, credential, or production ACK is invented in the
  default target composition.

## Fresh verification

| Check | Result |
| --- | --- |
| PlatformIO native | PASS, 346/346 |
| Native ASan/UBSan | PASS, 346/346 |
| cppcheck (`--fail-on-defect=low`) | PASS, no defects |
| `sigap_n16r8` | PASS, RAM 296,024/327,680; flash 492,577/4,194,304 |
| `sigap_n16r8_sd` | PASS, RAM 296,572/327,680; flash 565,957/4,194,304 |
| `sigap_n16r8_sd_spi` | PASS, RAM 296,316/327,680; flash 545,265/4,194,304 |
| `sigap_bench` | PASS, RAM 296,024/327,680; flash 492,577/4,194,304 |
| Go telemetry tests | PASS |
| Go full tests | PASS |
| `go vet ./...` | PASS |
| Draft 2020-12 schema compilation | PASS, all 14 schemas |
| Valid fixture corpus | PASS, 24/24 |
| Invalid fixture corpus | PASS, 36/36 expected errors |
| Cross-language record hash | PASS, `sha256:a3a2593618943cc7a1d5f8bc0c90216dc22eb4c3297fb874887c11b7b6ef6883` |
| Exact request-body hash | PASS, `sha256:3620edf0548b8a15eb1638fc7d239b1995372a28982f7f1e194949160f166c7c` |
| `git diff --check` | PASS |

The targeted schema run executed `TestAllTelemetrySchemasCompile`, 24 valid
fixtures, 36 invalid fixtures, and the frozen record/body hash tests. The
sanitizer run used the same source revision and an isolated PlatformIO build
directory with address and undefined-behavior sanitizers.

## Resource notes

The target map reports the `SigapApp` singleton at 276,376 bytes in the
N16R8 DRAM image and total static RAM at 296,024 bytes. The network worker
keeps only bounded token/retry state and references to archive-owned objects;
the HTTPS adapter bounds request bodies at 32 KiB and response bodies at
512 bytes. No dynamic allocation was introduced by the network worker.

Compiler stack-usage files are not emitted by the pinned PlatformIO release;
the target map and native sanitizer checks are therefore evidence for static
footprint and memory safety, not a hardware stack high-water measurement.

## Hardware boundary

No network credentials, endpoint, CA bundle, or backend service was supplied
for this slice. Native USB/SD evidence belongs to Task 6 and is unchanged.
Network association, TLS handshake, server ACK, Wi-Fi-loss timing, and
throughput remain HIL/deployment checks for a provisioned device. No upload,
reboot, SD modification, sensor wiring, or actuator test was performed here.

## Provenance

Task 7 source is based on the Task 6 merge above. Task 7 remains open for
review; Task 8 and all deferred BLE/backend/actuator work were not started.
