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
| target batch index | `FsImmutableBatchStore`, entries | 64 committed batch identities | firmware build; rebuilt on restart |

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
or reclaim pending data on the failed primary. Migration, if later needed,
must use copy, verify, checkpoint, and reclaim.

`ImmutableBatch` freezes the exact request body and SHA-256 before the first
attempt. `FsImmutableBatchStore` stores device identity, record count, body
hash, and exact bytes, rebuilds a bounded identity index after restart, and
provides a bounded replay operation. `AckJournal` and the target `FsAckJournalStore` advance only for a
matching durable server ACK including device identity, body hash, and counts.
Timeout, conflict, invalid ACK, and summary-v2 ACK do not advance the
delivered state; reclaim requires an explicit checkpoint.

Native tests use memory stores, a filesystem test double, and fault injection.
They are not physical SD, power-cut, wear, throughput, or HIL evidence.
