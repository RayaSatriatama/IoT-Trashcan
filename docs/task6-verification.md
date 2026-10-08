# Task 6 verification report

## Scope

This report covers the durable archive/logger and immutable batching slice on
the `feat/firmware-durable-archive` branch. Task 5 was merged before this
branch was created. No physical SD write, firmware upload, reboot, power-cut,
sensor, heater, or network test was performed.

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

## Fresh native evidence (final remediation run)

The integrated native suite passed **304/304 tests**, including archive storage,
archive runtime, immutable batch, ACK, failover, corrupt-tail, quota, 129-entry
cache overflow, 4,684-record workload, migration adapter, gap reserve failover,
producer-to-archive retry, typed metadata decode, and runtime batch/ACK tests.
The final run also covers ACK-to-manifest replay after runtime recreation. The
actual C++ builder output for sample, result, event, and metadata records was
independently accepted by the existing Go batch validator. The telemetry
contract tests passed their frozen canonical record and exact-body hash
fixtures.

The complete native suite was rerun with AddressSanitizer and UndefinedBehaviorSanitizer
through a temporary native PlatformIO configuration: **304/304 tests**, with
no sanitizer diagnostics. The sanitizer environment is temporary and is not part of the production
PlatformIO configuration. The final regression is covered by the focused
native run and the final full native run.
Cppcheck reported `No defects found`.

Target compilation passed for `sigap_n16r8`, `sigap_n16r8_sd`, and
`sigap_bench`. Final usage was 300,272 B RAM and 486,597 B flash for
`sigap_n16r8`, 300,556 B RAM and 536,897 B flash for `sigap_n16r8_sd`, and
300,272 B RAM and 486,597 B flash for `sigap_bench`. The target composition
uses one owned storage handoff slot; upstream typed rings retain additional
records, keeping the SD variant below the internal DRAM limit. These are compilation
evidence only, not physical N16R8 or SD I/O validation.

Native size probes continue to treat record/batch byte arrays and journal
capacity as explicit bounded ownership. `SampleRing<16>` is 5,088 B after
adding first/last dropped identities for queue gap evidence. The large native
fault-injection stores are not instantiated in the target composition.
`ArchiveRuntime` is 35,976 B, including two instance-owned 16,488 B record
codec buffer; no equivalent 16 KiB automatic buffer remains on the application
loop stack. `ImmutableRecordBytes` is 16,488 B, `ImmutableBatch` is 32,936 B,
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
step. Native filesystem-double regressions cover the same adapter boundary,
including source retention on checkpoint failure. Physical replay and
copy-verify-checkpoint-reclaim remain unverified because physical SD I/O is
prohibited in this batch.

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
batch, ACK, and migration adapters are exercised through native filesystem
doubles and fault injection, not the physical card. The cooperative worker
keeps filesystem work outside acquisition callbacks and bounds migration to
one budgeted step per poll; target stack high-water and physical latency are
not claimed. HTTPS transport, backend ACK production, network workers,
backend archive, migration 005, and actuator output are out of scope.
