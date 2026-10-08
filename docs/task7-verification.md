# Task 7 network, Wi-Fi, BLE, and web verification

## Scope

Task 6 was merged into `main` at `57d0749632f29b18fd1086b893868a5612b7c935`.
This branch adds the bounded Wi-Fi/HTTPS archive-v3 transport foundation,
always-on encrypted BLE provisioning, device-side Wi-Fi scanning, and the web
Bluetooth settings flow. It does not add the legacy summary worker, backend
archive code, migration 005, or actuator output.

Final verified source is updated after the integrated remediation commit. The
working tree includes the HTTP publication boundary, NimBLE passkey/authentication
callback, ATT-safe request/response framing, explicit Wi-Fi retry/list commands,
and browser status polling.

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
- BLE advertises a device-specific `SIGAP-XXXX` name continuously. Its command
  and response characteristics require NimBLE encrypted/authenticated GATT
  with a device-local passkey derived from a separate NVS setup secret. `F1` /
  `R1` request-ID frames are reassembled in order with a 20-byte wire bound.
  `GET_STATUS`, `SCAN_WIFI`, `LIST_KNOWN_WIFI`, `RETRY_WIFI`, and
  `CONNECT_WIFI` are bounded and session-scoped.
- Wi-Fi candidates are held in RAM until association and IP acquisition. A
  failed candidate preserves the previous saved credential. BLE remains
  available while Wi-Fi connects, retries, or is already connected.
- The web device detail page opens the Web Bluetooth picker from a user
  gesture, rejects unsupported/non-secure contexts, and sends credentials only
  through the BLE characteristic. Browser storage and API forwarding are not
  used.

## Fresh verification

| Check | Result |
| --- | --- |
| PlatformIO native | PASS, 365/365 |
| Native ASan/UBSan | PASS, 365/365 |
| cppcheck (`--fail-on-defect=low`) | PASS, no defects |
| `sigap_n16r8` | PASS, RAM 49,496/327,680; flash 1,073,557/4,194,304 |
| `sigap_n16r8_sd` | PASS, RAM 50,036/327,680; flash 1,147,337/4,194,304 |
| `sigap_n16r8_sd_spi` | PASS, RAM 49,792/327,680; flash 1,126,773/4,194,304 |
| `sigap_bench` | PASS, RAM 49,496/327,680; flash 1,073,557/4,194,304 |
| Web Vitest | PASS, 87/87 |
| Web TypeScript | PASS |
| Web production export | PASS |
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

The final native additions cover monotonically rejected BLE request replays,
bounded scan-count serialization, readback-verified Wi-Fi secret persistence,
and the empty known-network list during first setup. The sanitizer run covered
the same 365 native cases.

## Resource notes

The target composition owns a large archive/application object. With Wi-Fi and
NimBLE enabled, placing that object in internal DRAM overflowed the linker by
about 50 KiB. The final composition allocates the object from the board's
verified PSRAM pool after framework startup; only the pointer remains in DRAM.
PlatformIO reports 49,496 bytes static RAM for the base target and 50,036 bytes
for SD_MMC. The network worker keeps bounded token/retry state and references
archive-owned bytes. HTTPS bounds request bodies at 32 KiB and response bodies
at 512 bytes. Runtime PSRAM capacity remains a board acceptance condition and
is reported by the existing memory probe; no physical boot was performed in
this verification pass.

Compiler stack-usage files are not emitted by the pinned PlatformIO release;
the target map and native sanitizer checks are therefore evidence for static
footprint and memory safety, not a hardware stack high-water measurement.

## Hardware boundary

No network endpoint, CA bundle, backend service, or physical BLE client was
used in this pass. Wi-Fi credentials supplied in conversation were not written
to source, tests, logs, or artifacts. Native USB/SD evidence belongs to Task 6
and is unchanged. BLE advertising/pairing, device-side scan results, TLS
handshake, server ACK, Wi-Fi-loss timing, throughput, and credential
persistence remain HIL/deployment checks for a provisioned device. No upload,
reboot, SD modification, sensor wiring, or actuator test was performed here.

## Provenance

Task 7 source is based on the Task 6 merge above. Task 8 backend work,
migration 005, durable logger changes, FIS/Safety recalculation, and actuator
output were not started. Physical commissioning remains deferred.
