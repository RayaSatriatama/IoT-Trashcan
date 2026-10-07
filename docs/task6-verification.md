# Task 6 verification report

## Scope

This report covers the durable archive/logger and immutable batching slice on
the `feat/firmware-durable-archive` branch. Task 5 was merged before this
branch was created. No physical SD write, firmware upload, reboot, power-cut,
sensor, heater, or network test was performed.

## Implemented evidence

- The logger owns acquisition handoff consumption. Acquisition callbacks only
  perform bounded observer/queue work; filesystem encoding, open, write, and
  flush occur in the application drain loop.
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
- `StorageWorker` provides an instance-owned bounded retry queue with
  backpressure, high-water, terminal-gap, and slow-backend counters. Its
  worker API is covered natively; the existing sample/result drain remains
  the compatibility boundary for the current application composition.

## Fresh native evidence

The integrated native suite passed 284/284 tests, including archive storage,
archive runtime, immutable batch, ACK, failover, corrupt-tail, quota, 129-entry
cache overflow, 4,680-record workload, migration adapter, gap reserve failover, and
producer-to-archive retry tests. The existing telemetry contract tests also
passed their frozen canonical record and exact-body hash fixtures.

The changed archive/runtime, batch, storage-worker, and migration tests were
rerun with AddressSanitizer and UndefinedBehaviorSanitizer through a temporary
native PlatformIO configuration: 9 + 19 + 4 + 3 = 35 tests, with no sanitizer
diagnostics. A full sanitizer run is not represented as a single suite because
the repository's alternate PlatformIO project configuration cannot map every
native test environment at once.
Cppcheck reported `No defects found`.

Target compilation passed for `sigap_n16r8`, `sigap_n16r8_sd`, and
`sigap_bench`. Final usage was 222,528 B RAM and 456,145 B flash for
`sigap_n16r8`, 222,812 B RAM and 506,325 B flash for `sigap_n16r8_sd`, and
222,528 B RAM and 456,145 B flash for `sigap_bench`. These are compilation
evidence only, not physical N16R8 or SD I/O validation.

Native size probes continue to treat record/batch byte arrays and journal
capacity as explicit bounded ownership. `SampleRing<16>` is 4,656 B after
adding first/last dropped identities for queue gap evidence. The large native
fault-injection stores are not instantiated in the target composition.
`ArchiveRuntime` is 35,976 B, including two instance-owned 16,488 B record
codec buffer; no equivalent 16 KiB automatic buffer remains on the application
loop stack. `ImmutableRecordBytes` is 16,488 B, `ImmutableBatch` is 32,936 B,
`GapTracker<32>` is 2,840 B, and the native `MemoryAckJournalStore<2>` is
336 B. `FsAckJournalStore` is target-only and is therefore not represented by
the native layout measurement; its bounded target entries and frame scratch
remain instance-owned.

## Budget measurement

The native serialized workload test appends and reconstructs 4,680 bounded
sample frames against the 6 MiB quota. It verifies every identity and hash
after restart without overwriting pending data. It is not a mixed
75-sample-plus-3-result workload: result/event/metadata payload sizes and their
combined 60-second capacity remain an acceptance gap rather than evidence.
This is a serialized native capacity measurement, not a claim of physical
flash or SD endurance.

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

The full archive-reader to telemetry-v3 JSON batch-builder to immutable batch
store to ACK-application producer chain is not yet runtime-wired. The batch
and ACK stores remain bounded, validated persistence boundaries with native
coverage; no unconnected producer hook is presented as delivery evidence.
Likewise, `StorageWorker` is currently a tested bounded ownership seam rather
than a second FreeRTOS task in `SigapApp`; acquisition and processing drains
retain their existing compatibility behavior. Network transport and its
producer scheduling are deferred to the next transport slice.

## Deferred

Physical SD mount/read/write, crash/power-cut tail loss, wear, and physical
copy-verify-checkpoint-reclaim migration remain deferred. The target ACK
journal and record archive boundaries are compiled and wired, but are not
exercised against physical media. The cooperative application loop keeps
filesystem work outside acquisition callbacks and bounds migration to one
record per poll; dedicated RTOS storage-worker timing and physical endurance
evidence are deferred to hardware validation. HTTPS transport, backend ACK
integration, network workers, backend archive, migration 005, and actuator
output are out of scope.
