# Task 6 verification report

## Scope

This report covers the durable archive/logger and immutable batching slice on
the `feat/firmware-durable-archive` branch. Task 5 was merged before this
branch was created. No firmware upload, reboot, power-cut, sensor, heater, or
network test was performed. Physical SD I/O was not executed in this batch;
the corrected image is prepared for a separately authorized upload and
attached-only SD probe.

## Implemented evidence (final remediation)

- The logger owns acquisition handoff consumption. Acquisition callbacks only
  perform bounded observer/queue work; immutable records are copied into an
  instance-owned `StorageWorker` queue and filesystem encoding, open, write,
  and flush occur only from the bounded storage poll.
- Processing results remain in their bounded handoff until archive append
  returns `Durable`.
- Framing uses a fixed little-endian layout with magic/version, bounded length,
  identity, CRCs, payload, and commit marker.
- In-memory fault injection covers short write, flush failure, media failure,
  quota block, corrupt tail, duplicate identity, and recovery.
- SD and LittleFS adapters never auto-format. `MediaFailoverStore` retains the
  failed primary state while routing new writes to the fallback.
- Immutable batch bytes and body hash cannot be replaced after preparation.
- ACK status, device identity, batch identity, body hash, record count, and
  counts must match. The target `FsAckJournalStore` persists pending, ACKed,
  and reclaimed journal operations; checkpoint precedes reclaim. No network
  worker is connected in Task 6.
- Gap ranges include device/boot/channel/time metadata where a sample identity
  exists. Retryable writes remain pending and do not create permanent gaps.
  Queue overflow records the complete dropped count with bounded first/last
  range evidence, and invalid records are terminally observable.
- Sample frames have a bounded typed decoder, including monotonic timestamp,
  quality/reason, raw evidence, converted fields, and profile provenance.
- Target archive and ACK paths rebuild bounded identity/state indexes after
  restart. ACK frames include opcode validation, CRC, and a checkpoint
  generation binding. Same-identity different-hash appends are conflicts.
- `ArchiveMediaMigration` performs append, identity/hash verification, durable
  checkpoint, and source reclaim only after a durable receipt. The application
  loop exposes one bounded migration step through `ArchiveRuntime` without
  performing filesystem work in acquisition callbacks.
- Audit events remain at the front of their producer ring until the encoded
  event append is durable. The encoded bytes and shared sequence are frozen
  across retry, and terminal invalid events are accounted as gaps.
- `StorageWorker` is an active runtime consumer for samples, processing
  results, and encoded audit events. It provides an instance-owned bounded
  retry queue with backpressure, high-water, terminal-gap, and slow-backend
  counters. `Queued` transfers ownership while `Durable` is returned only
  after backend append succeeds. Audit events remain in their producer ring
  until the worker reports durable or terminal completion.
- `ArchiveBatchRuntime` is bound in `SigapApp`: archive reader -> typed
  sample/result/event/metadata decoder -> canonical v3 one-record immutable
  batch -> durable batch store -> pending ACK journal. A valid ACK injected at
  the transport boundary is checkpointed before archive reclaim; invalid or
  summary ACKs cannot advance delivery. No production ACK is synthesized.
  After runtime recreation, a durable ACK is matched against the exact pending
  archive frame before reclaim; replaying an ACK for another manifest leaves
  the current record pending.
- The composition root binds LittleFS archive, SD archive, failover, and the
  media migration source/destination. Boot metadata is produced only when a
  provisioned device identity is available; no identity is fabricated.
- Production `#ifdef ARDUINO` archive, batch, ACK, and migration paths are
  executed through a filesystem test double. The focused adapter suite covers
  failed recovery copy, failed rename, short read/write, marker recovery,
  restart reconstruction, exact-byte ACK replay, and migration receipt
  persistence.
- A bounded target maintenance command, `storage sd test`, is implemented for
  the SD target. It mounts without formatting, writes one deterministic
  256-byte file, flushes/closes, reopens, verifies size and SHA-256, reports
  timing/capacity, and removes only its own test file. It is target-compiled
  but has not been run on the board image currently installed.

## Fresh native evidence (final remediation run)

The integrated native suite passed **328/328 tests**, including archive storage,
archive runtime, immutable batch, ACK, failover, corrupt-tail, quota, 129-entry
cache overflow, 4,684-record workload, migration adapter, gap reserve failover,
producer-to-archive retry, typed metadata decode, and runtime batch/ACK tests.
The final run also covers ACK-to-manifest replay after runtime recreation. The
actual C++ builder output for sample, result, event, and metadata records was
independently accepted by the existing Go batch validator. The telemetry
contract tests passed their frozen canonical record and exact-body hash
fixtures.

The complete native suite was rerun with AddressSanitizer and UndefinedBehaviorSanitizer
through a temporary native PlatformIO configuration: **328/328 tests**, with
no sanitizer diagnostics. The sanitizer environment is temporary and is not part of the production
PlatformIO configuration. The final regression is covered by the focused
native run and the final full native run.
Cppcheck reported `No defects found`.

Target compilation passed for `sigap_n16r8`, selected `sigap_n16r8_sd`,
explicit `sigap_n16r8_sd_spi`, and `sigap_bench`. Final usage was:

| Target | RAM | Flash |
| --- | ---: | ---: |
| `sigap_n16r8` | 283,864 B | 489,745 B |
| `sigap_n16r8_sd` (SdMmc1Bit) | 284,404 B | 562,553 B |
| `sigap_n16r8_sd_spi` (ExternalSpi) | 284,148 B | 541,917 B |
| `sigap_bench` | 283,864 B | 489,745 B |

The selected SD_MMC image is below the 327,680-byte internal DRAM limit.
These are compilation/map measurements only, not physical SD I/O validation.

Native size probes continue to treat record/batch byte arrays and journal
capacity as explicit bounded ownership. `SampleRing<16>` is 5,088 B after
adding first/last dropped identities for queue gap evidence. The large native
fault-injection stores are not instantiated in the target composition.
`ArchiveRuntime` now owns one 16,488 B record codec buffer plus a small audit
identity, rather than a second redundant 16,488 B audit frame. The shared
scratch is protected by explicit audit backpressure, and the regression test
proves a pending audit cannot be overwritten by a sample. No equivalent 16 KiB
automatic buffer remains on the application loop stack. `ImmutableRecordBytes`
is 16,488 B, `ImmutableBatch` is 32,936 B,
`GapTracker<32>` is 2,840 B, and the native `MemoryAckJournalStore<2>` is
336 B. `StorageWorker<1>` is instance-owned in the target composition and
stores one owned frozen frame plus bounded status counters. Native
fault-injection tests still exercise larger worker queues independently.
`FsAckJournalStore` is
target-only and is therefore not represented by the native layout measurement;
its bounded target entries and frame scratch remain instance-owned.

## Budget measurement

The native serialized workload test appends and reconstructs a mixed 60-second
fixture of 4,500 sample frames, 180 result frames, and 4 event frames (4,684
records) against the 6 MiB quota. Every identity/hash is reconciled after
restart without overwriting pending data. A separate integrated fixture uses a
representative BOOT metadata payload and result trace through the typed decoder
and batch builder. This is serialized native capacity evidence, not a claim of
physical flash or SD endurance; physical media throughput/endurance remains
unverified.

The archive runtime regression also verifies that a result containing FIS and
Safety evidence serializes differently from the same result without that
evidence. The native filesystem double verifies restart reconstruction after a
partial/corrupt tail, append-boundary recovery, and identity/hash index rebuild.

The target batch adapter provides bounded exact-byte replay after runtime
recreation and writes a durable checkpoint marker. `SigapApp` now owns and
wires the LittleFS source, SD destination, and `ArchiveMediaMigration` into
`ArchiveRuntime`; each poll performs at most one copy/verify/checkpoint/reclaim
step. Recovery markers are the only tail-repair authority; unmarked quarantine
and post-reclaim backup files are not selected as pending data. Native
filesystem-double regressions cover source retention on checkpoint failure and
receipt retry/restart behavior. Physical replay and
copy-verify-checkpoint-reclaim remain unverified because the corrected image
has not been uploaded in this batch.

The archive-reader to telemetry-v3 JSON batch-builder to immutable batch store
to ACK-application chain is runtime-wired and natively fault-tested. The
current implementation intentionally uses one bounded record per immutable
batch to cap workspace and preserve exact retry bytes. `StorageWorker` is a
cooperative instance-owned worker polled with a bounded budget from the
application loop, not a second FreeRTOS task. This keeps filesystem ownership
out of acquisition callbacks; dedicated RTOS scheduling and physical timing
evidence remain hardware work. Network transport and backend ACK production
remain out of Task 6.

## Deferred

Physical SD mount/read/write, crash/power-cut tail loss, wear, and physical
copy-verify-checkpoint-reclaim migration remain deferred. Target filesystem,
batch, ACK, and migration adapters are exercised through the production
`#ifdef ARDUINO` code with a native filesystem double and fault injection, not
the physical card. The cooperative worker
keeps filesystem work outside acquisition callbacks and bounds migration to
one budgeted step per poll; target stack high-water and physical latency are
not claimed. HTTPS transport, backend ACK production, network workers,
backend archive, migration 005, and actuator output are out of scope.

## Hardware inventory and SD prerequisite

The native maintenance interface is the Espressif USB JTAG/serial debug unit
(`303A:1001`) at the stable by-id path observed as `/dev/ttyACM0`. The
confirmed PKM KC onboard slot is selected by `sigap_n16r8_sd` with SD_MMC
1-bit pins CLK=39, CMD=38, D0=40, no chip select, and
`format_if_mount_failed=false`. The bounded `storage sd test` command is
available after the corrected image is uploaded and rebooted. No format or
destructive operation is required.

## SD transport reconciliation

The user-confirmed installed board follows the PKM KC ESP32-S3-N16R8 CAM
topology using SD_MMC 1-bit (`CLK=39`, `CMD=38`, `D0=40`,
`format_if_mount_failed=false`). The selected `sigap_n16r8_sd` target now
enables this policy. GPIO38 remains reserved by the generic SIGAP safety map
and is allowed only in the explicit `SdMmc1Bit` policy. The external SPI
variant remains available as `sigap_n16r8_sd_spi` (`SCK=39`, `MISO=40`,
`MOSI=41`, `CS=43`).

A prior SD_MMC link failed by 3,048 bytes. The linker maps identified the
precise cause: SD_MMC-required IRAM increased `.dram0.dummy` from `0xb0cc` to
`0xbbbc` (+2,800 B), with `.dram0.data` +112 B and `.dram0.bss` +144 B; only
8 B remained, producing the reported overflow. This was not a physical-heap
measurement. The remediation removes the redundant target audit frame buffer
and serializes SD_MMC without unused high-level `SD`/`SPI` libraries
(`lib_ignore = SD, SPI`); required SD_MMC integrity, recovery, quota, and
workload paths remain unchanged. The resulting selected build uses 284,404 B
RAM and links successfully.

Compiler stack-usage instrumentation on the selected target produced 71 `.su`
files. Largest relevant static frame estimates were
`ProcessingRuntime::evaluate_cycle` 14,064 B, `BatchJsonBuilder::write_result`
9,232 B, and `ProcessingPipeline::evaluate` 7,168 B. Archive recovery and
storage functions remained bounded (for example, `FsImmutableBatchStore::recover`
976 B, `FsAckJournalStore::recover` 416 B, `ArchiveMediaMigration::migrate_one`
160 B, and `StorageWorker::process_one` 192 B). These are compiler estimates,
not measured runtime stack high-water values; physical stack telemetry remains
HIL work.

The reproducible selected image is
`.pio/build/sigap_n16r8_sd/firmware.bin`, SHA-256
`cc35859400fe3f2aa262a68643a298a2bfe33640d680b5e90926fe4f8cb70ac1`.

Fresh final verification: native PlatformIO 328/328, ASan/UBSan 328/328,
cppcheck no defects, and all four firmware environments above passed. The
cached official Go 1.27.1 toolchain was invoked explicitly: telemetry tests,
full Go tests, and `go vet ./...` passed. Schema tests compiled all 14 schemas,
validated 24 valid fixtures, rejected the 36 structural invalid fixtures, and
the canonical record and exact-body golden hashes remained unchanged.
