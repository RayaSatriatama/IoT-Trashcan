# Task 6 verification report

## Scope

This report covers the durable archive/logger and immutable batching slice on
the `feat/firmware-durable-archive` branch. Task 5 was merged before this
branch was created. The selected SD_MMC image was uploaded through native
USB without mass erase; a controlled firmware reset occurred as part of that
upload. Attached-only SD verification was then run without formatting,
unplugging, power-cut testing, or sensor/heater hardware.

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
  the SD target. It mounts without formatting, verifies a durable archive
  append/reopen/readback/reclaim cycle, writes one deterministic 256-byte
  file, flushes/closes, reopens, verifies size and SHA-256, reports
  timing/capacity, and removes only its own test file. SD transport setup is
  idempotent after application startup, so the probe does not call
  `SD_MMC.setPins()` a second time.

## Fresh native evidence (final remediation run)

The integrated native suite passed **331/331 tests**, including archive storage,
archive runtime, immutable batch, ACK, failover, corrupt-tail, quota, 129-entry
cache overflow, 4,684-record workload, migration adapter, gap reserve failover,
producer-to-archive retry, typed metadata decode, and runtime batch/ACK tests.
The final run also covers ACK-to-manifest replay after runtime recreation. The
actual C++ builder output for sample, result, event, and metadata records was
independently accepted by the existing Go batch validator. The telemetry
contract tests passed their frozen canonical record and exact-body hash
fixtures.

The complete native suite was rerun with AddressSanitizer and UndefinedBehaviorSanitizer
through a temporary native PlatformIO configuration: **331/331 tests**, with
no sanitizer diagnostics. The sanitizer environment is temporary and is not part of the production
PlatformIO configuration. The final regression is covered by the focused
native run and the final full native run.
Cppcheck reported `No defects found`.

Target compilation passed for `sigap_n16r8`, selected `sigap_n16r8_sd`,
explicit `sigap_n16r8_sd_spi`, and `sigap_bench`. Final usage was:

| Target | RAM | Flash |
| --- | ---: | ---: |
| `sigap_n16r8` | 295,784 B | 489,941 B |
| `sigap_n16r8_sd` (SdMmc1Bit) | 296,332 B | 563,373 B |
| `sigap_n16r8_sd_spi` (ExternalSpi) | 296,068 B | 542,693 B |
| `sigap_bench` | 295,784 B | 489,941 B |

The selected SD_MMC image is below the 327,680-byte internal DRAM limit.
The selected SD_MMC target leaves 31,348 B of internal DRAM below the
327,680-byte limit. These are compilation/map measurements; physical SD
evidence is recorded separately below.

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
receipt retry/restart behavior. The attached board additionally verified the
production SD archive append/reopen/recover/contains/reclaim path; physical
crash/power-cut replay and migration remain unverified.

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

Attached-only SD mount/read/write and production archive append/reopen/recover/
reclaim were verified on the connected board through the production
`#ifdef ARDUINO` path. The bounded probe wrote and removed only its own
`/sigap/task6_sd_probe.bin` file, without formatting or destructive recovery.
Crash/power-cut tail loss, wear, and physical copy-verify-checkpoint-reclaim
migration remain unverified. The cooperative worker keeps filesystem work
outside acquisition callbacks and bounds migration to one budgeted step per
poll; target stack high-water and physical latency are not claimed. HTTPS
transport, backend ACK production, network workers, backend archive, migration
005, and actuator output are out of scope.

## Hardware inventory and SD prerequisite

The native maintenance interface is the Espressif USB JTAG/serial debug unit
(`303A:1001`) at the stable by-id path observed as `/dev/ttyACM0`. The
confirmed PKM KC onboard slot is selected by `sigap_n16r8_sd` with SD_MMC
1-bit pins CLK=39, CMD=38, D0=40, no chip select, and
`format_if_mount_failed=false`. The bounded `storage sd test` command was run
on the corrected image. No format or destructive operation was performed.

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
workload paths remain unchanged. The final selected build uses 296,332 B RAM
and links successfully.

Compiler stack-usage instrumentation on the selected target produced 71 `.su`
files. The processing workspace remediation reduced the relevant call-chain
frames to `ProcessingRuntime::evaluate_cycle` 224 B,
`ProcessingPipeline::evaluate` 5,376 B, and `SlotProcessor::process` 1,664 B
(7,264 B conservative chain total, below the 8,192 B Arduino loop-task
budget). `BatchJsonBuilder::write_result` is 208 B and `build` is 272 B.
Archive recovery and storage functions remained bounded, including
`LittleFsArchiveStore::recover` 368 B, `recover_marked_quarantine` 256 B,
and `StorageWorker::process_one` 192 B. These are compiler estimates, not
measured runtime stack high-water values; physical stack telemetry remains
HIL work.

The reproducible selected image is
`.pio/build/sigap_n16r8_sd/firmware.bin`, SHA-256
`3f8debe88b4d9f5e433fb2a2f83949e2b4fb37c613e500bca2e6399065807a6d`.

Fresh final verification: native PlatformIO 331/331, ASan/UBSan 331/331,
cppcheck no defects, and all four firmware environments above passed. The
cached official Go 1.27.1 toolchain was invoked explicitly: telemetry tests,
full Go tests, and `go vet ./...` passed. Schema tests compiled all 14 schemas,
validated 24 valid fixtures, rejected the 36 structural invalid fixtures, and
the canonical record and exact-body golden hashes remained unchanged.

### Attached-only SD evidence

Final image upload used native USB at
`/dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_98:A3:16:F5:C1:50-if00`;
NVS and credentials were preserved. The board reported 16,777,216-byte flash,
8,388,608-byte physical PSRAM, `status=MATCH`, config version 1, and a nonzero
config hash. The attached `storage sd test` returned:

```text
sd=PASS
bus=mmc1
n=256
hash=f987a1b5e0b87fc9e660f09967563a78de60df01dec65236945120eea61d4c56
us=210674
t=31252201472
u=49152
f=31252152320
a=PASS
OK
```

This proves selected SD_MMC mount, archive append/reopen/recover/contains/
reclaim, bounded write/flush/close/reopen/readback/hash, and cleanup of the
probe file. The logged `*.recover` open messages are expected empty recovery
probes and did not prevent the explicit `PASS`; no card formatting, unplug,
power-cut, or sensor/heater test was performed.
