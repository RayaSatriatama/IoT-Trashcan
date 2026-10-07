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

## Fresh native evidence

The integrated native suite passed 263/263 tests, including archive storage,
archive runtime, immutable batch, ACK, failover, corrupt-tail, quota, and
producer-to-archive retry tests. The existing telemetry contract tests also
passed their frozen canonical record and exact-body hash fixtures.

The final native suite was rerun after target adapter compilation was forced
through `archive_runtime.h`. The sanitizer copy passed 262/262 tests with
AddressSanitizer and UndefinedBehaviorSanitizer and produced no diagnostics;
a final focused sanitizer run passed 22/22 archive, batch, and migration tests.
Cppcheck reported `No defects found`.

Target compilation passed for `sigap_n16r8`, `sigap_n16r8_sd`, and
`sigap_bench`. Final usage was 123,192 B RAM and 450,681 B flash for
`sigap_n16r8` and `sigap_bench`, and 123,476 B RAM and 500,829 B flash for
`sigap_n16r8_sd`. These are compilation evidence only, not physical N16R8 or
SD I/O validation.

Native size probes continue to treat record/batch byte arrays and journal
capacity as explicit bounded ownership. `SampleRing<16>` is 4,656 B after
adding first/last dropped identities for queue gap evidence. The large native
fault-injection stores are not instantiated in the target composition.
`ArchiveRuntime` is 19,104 B, including one instance-owned 16,488 B record
codec buffer; no equivalent 16 KiB automatic buffer remains on the application
loop stack. `ImmutableBatch` is 32,936 B, `GapTracker<32>` is 2,584 B, and
the native `MemoryAckJournalStore<2>` is 65,896 B.

## Budget measurement

The native serialized workload test appends 80 bounded frames: 75 sample
frames, 3 representative result frames, 1 event frame, and 1 metadata frame.
This approximates the TSD initial 75 sample frames/s plus 3 processing results/s
and verifies bounded quota accounting and no overwrite. This is a serialized workload
measurement, not a claim of 60 seconds of physical flash or SD endurance.

The archive runtime regression also verifies that a result containing FIS and
Safety evidence serializes differently from the same result without that
evidence. The native filesystem double verifies restart reconstruction after a
partial/corrupt tail, append-boundary recovery, and identity/hash index rebuild.

The target batch adapter provides bounded exact-byte replay after runtime
recreation and writes a durable checkpoint marker. Native migration regressions
cover verify/checkpoint ordering and source retention on checkpoint failure.
Physical replay and copy-verify-checkpoint-reclaim migration remain unverified
because physical SD I/O is prohibited in this batch. The application loop has a
bounded migration hook; target source/destination wiring remains a follow-up.

## Deferred

Physical SD mount/read/write, crash/power-cut tail loss, wear, and physical
copy-verify-checkpoint-reclaim migration remain deferred. The target ACK
journal and record archive boundaries are compiled but not exercised against
physical media. HTTPS transport, backend ACK integration, network workers,
backend archive, migration 005, and actuator output are out of scope.
