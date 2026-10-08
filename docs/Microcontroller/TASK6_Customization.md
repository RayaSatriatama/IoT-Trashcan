# Task 6 archive and batching customization

Task 6 adds the bounded local archive boundary. It does not add HTTP, TLS,
backend archive code, or migration 005.

| Parameter | Owner and unit | Default/bound | Apply policy |
| --- | --- | --- | --- |
| `storage.flash_pending_quota_bytes` | `AppConfig`, bytes | validated factory value, reserve enforced | draft, validate, preview, commit, reboot |
| `storage.sd_pending_quota_bytes` | `AppConfig`, bytes | validated factory value | draft, validate, preview, commit, reboot |
| archive frame version and magic | `storage/archive_codec.h` | protocol constants | compile-time, not runtime configuration |
| record maximum | archive codec, bytes | 16 KiB contract bound | compile-time contract |
| batch maximum | immutable batch, bytes | 32 KiB contract bound | compile-time contract |
| journal/gap capacity | instance-owned rings | bounded static capacity | firmware build; no mutable global |
| gap reserve journal | `LittleFsArchiveStore` / `MediaFailoverStore`, bytes | 4 KiB outside pending-record quota | append after terminal gap decision; target recovery rejects malformed frame |
| target archive identity cache | `LittleFsArchiveStore`, entries | 128 hot identities; full archive remains file-backed and scans cache misses | firmware build; rebuilt on restart |
| target batch index | `FsImmutableBatchStore`, entries | 64 hot batch identities | firmware build; rebuilt on restart |
| migration work budget | `ArchiveMediaMigration` / `ArchiveRuntime`, records per loop | one bounded record step | instance-owned; retryable until verify, checkpoint, and reclaim succeed |
| storage worker queue | `storage/storage_worker.h`, entries | target composition uses one owned frozen-frame slot; native fault tests exercise larger bounded queues; high-water and overflow observable | instance-owned; retryable until durable, terminal gap on invalid input |
| ACK journal frame | `FsAckJournalStore`, bytes | fixed little-endian frame with CRC and generation-bound checkpoint | protocol constant; target recovery validates before state transition |
| `storage sd test` probe | maintenance/SigapApp, bytes and elapsed microseconds | one 256-byte deterministic test file; no formatting; dedicated path removed after verification | explicit command on `SIGAP_USE_SD` target; requires authorized upload/reboot before physical execution |
| SD transport policy | `sigap/board/sd_transport.h`, typed build policy | `ExternalSpi`: SCK/MISO/MOSI/CS 39/40/41/43; opt-in `SdMmc1Bit`: CLK/CMD/D0 39/38/40, 1-bit, no format | compile-time board/build selection; default remains ExternalSpi until the installed board topology is proven |

Calibration coefficients, capacity, and scientific model parameters remain
typed versioned profiles. GPIO and bus mapping remain board/build constants.
Credentials remain in `SecretStore`. Runtime media health, counters, pending
records, and ACK state are instance-owned runtime state.

The binary frame is little-endian and contains magic, format version, record
kind, bounded payload length, sequence/device/boot identity, payload CRC,
header CRC, payload, and a commit marker. A record is durable only after the
complete frame is written and flushed. Recovery accepts only complete frames;
a partial or corrupt tail is ignored and never exposed as a committed record.

SD is the primary adapter when mounted by the target composition. LittleFS is
the fallback. Neither adapter formats media. A fallback append does not delete
or reclaim pending data on the failed primary. The target composition binds
`ArchiveStoreMigrationSource` to LittleFS and
`ArchiveStoreMigrationDestination` to SD. Migration uses copy, identity/hash
verification, checkpoint, and source reclaim; the receipt binds source media,
destination media, identity, hash, and checkpoint generation. A destination
copy remains pending until a valid server ACK in a later task.

`ImmutableBatch` freezes the exact request body and SHA-256 before the first
attempt. `FsImmutableBatchStore` stores device identity, record count, body
hash, and exact bytes, rebuilds a bounded identity index after restart, and
provides a bounded replay operation. `AckJournal` and the target `FsAckJournalStore` advance only for a
matching durable server ACK including device identity, body hash, and counts.
Timeout, conflict, invalid ACK, and summary-v2 ACK do not advance the
delivered state; reclaim requires an explicit checkpoint.

Native tests use memory stores, a filesystem test double that exercises the
store migration adapters, and fault injection.
They are not physical SD, power-cut, wear, throughput, or HIL evidence.

The PKM KC reference uses an onboard ESP32-S3 CAM SD_MMC socket on
39/38/40. That is not evidence for the SIGAP DevKitC-1 target. GPIO38 remains
reserved by the generic SIGAP board safety map and is allowed only inside the
explicit opt-in `SdMmc1Bit` policy. The installed SIGAP board must be matched
to a schematic or clear wiring evidence before selecting that policy.

Audit events use the same ownership rule as samples and results: the producer
ring remains queued until the archive append is durable. A short write keeps
the event identity, sequence, provenance, and encoded bytes for retry.
`StorageWorker` is an active instance-owned consumer in `SigapApp`: drains do
only bounded encoding/enqueue work, while the worker owns filesystem
append/retry. `Queued` transfers ownership to the worker; `Durable` means the
backend append completed. The worker is cooperative rather than a second
FreeRTOS task in this slice, so target stack high-water and physical I/O
latency remain hardware evidence items.

`ArchiveBatchRuntime` is also bound by the composition root. It decodes the
typed archive frame, builds a canonical telemetry-v3 immutable body, persists
exact bytes, records a pending ACK journal entry, and applies a validated ACK
only after a durable checkpoint. Reclaim never follows an invalid,
summary-v2, mismatched, or uncheckpointed ACK. Native tests inject ACKs only
at the transport boundary; production does not synthesize server ACKs. After
runtime recreation, ACK application rebuilds the source binding from the exact
pending archive frame before reclaim, so a replayed ACK cannot reclaim the next
manifest.

The composition root binds LittleFS as the archive source, SD as the optional
destination, failover, and the copy/verify/checkpoint/reclaim migration
adapter. Boot metadata uses the shared sequence domain and is emitted only
when a provisioned device identity is present. No device identity or sensor
evidence is fabricated.
