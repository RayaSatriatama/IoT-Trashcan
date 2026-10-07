# Task 6 verification report

## Scope

This report covers the durable archive/logger and immutable batching slice on
the `feat/firmware-durable-archive` branch. Task 5 was merged before this
branch was created. No physical SD write, firmware upload, reboot, power-cut,
sensor, heater, or network test was performed.

## Implemented evidence

- Samples are observed without consuming the acquisition logger handoff.
- Processing results remain in their bounded handoff until archive append
  returns `Durable`.
- Framing uses a fixed little-endian layout with magic/version, bounded length,
  identity, CRCs, payload, and commit marker.
- In-memory fault injection covers short write, flush failure, media failure,
  quota block, corrupt tail, duplicate identity, and recovery.
- SD and LittleFS adapters never auto-format. `MediaFailoverStore` retains the
  failed primary state while routing new writes to the fallback.
- Immutable batch bytes and body hash cannot be replaced after preparation.
- ACK status, batch identity, body hash, record count, and counts must match;
  checkpoint precedes reclaim in the bounded journal seam. The current
  `AckJournal` is an in-memory/native boundary; persistent ACK journal
  integration with a network worker is intentionally not present in Task 6.
- Gap ranges and unlogged counters are bounded and observable.

## Fresh native evidence

The integrated native suite passed 253/253 tests, including archive storage,
archive runtime, immutable batch, ACK, failover, corrupt-tail, quota, and
producer-to-archive retry tests. The existing telemetry contract tests also
passed their frozen canonical record and exact-body hash fixtures.

The final native suite was rerun after target adapter compilation was forced
through `archive_runtime.h`. The sanitizer copy passed 253/253 tests with
AddressSanitizer and UndefinedBehaviorSanitizer and produced no diagnostics.
Cppcheck reported `No defects found`.

Target compilation passed for `sigap_n16r8`, `sigap_n16r8_sd`, and
`sigap_bench`. Final usage was 78,624 B RAM and 445,789 B flash for
`sigap_n16r8` and `sigap_bench`, and 78,908 B RAM and 495,869 B flash for
`sigap_n16r8_sd`. These are compilation evidence only, not physical N16R8 or
SD I/O validation.

Native size probes reported `ImmutableRecordBytes=16,488`,
`ArchiveRuntime=832`, `LittleFsArchiveStore=32`, `MediaFailoverStore=24`,
`ImmutableBatch=32,920`, `FsImmutableBatchStore=24`, and
`GapTracker<32>=792` bytes. The large `MemoryArchiveStore<32>` (527,656 B)
and `AckJournal<2>` (65,864 B) are native fault-injection harness objects and
are not instantiated in the target composition.

## Budget measurement

The native serialized workload test appends 78 bounded frames, representing
the TSD initial 75 sample frames/s plus 3 processing results/s. It verifies
bounded quota accounting and no overwrite. This is a serialized workload
measurement, not a claim of 60 seconds of physical flash or SD endurance.

## Deferred

Physical SD mount/read/write, crash/power-cut tail loss, wear, copy-verify-
checkpoint-reclaim migration, HTTPS transport, backend ACK integration, and
durable event or metadata producers remain deferred to later integration/HIL
work. Network workers, backend archive, migration 005, and actuator output are
out of scope. The current target archive path is record-durable after a
complete frame flush; batch ACK persistence and media migration still require
the next integration slice.
