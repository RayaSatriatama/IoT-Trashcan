# Prototype Operation (SIGAP_PROTOTYPE_OPERATION=1)

## Overview

`SIGAP_PROTOTYPE_OPERATION=1` now defaults to `SIGAP_LIVE_NO_BACKLOG=1`.
The owner-selected live policy is latest-reading freshness, not complete capture retention:

All four active physical sensors share a 1,000,000 us capture target and one
common latest-snapshot send cadence. SHT31 conversion completion is serviced
every 10 ms, separately from the one-second capture gate. Capture timestamps
stay original, not synchronized by rewriting them. This compile-time prototype
policy does not modify saved AppConfig/calibration. An active HTTPS attempt or
backoff may delay sending; missed periods are coalesced, never queued/replayed.
The device detail UI polls its shared current snapshot every 1,000 ms.

- No pending sensor archive in RAM, no SD backup, and no replay of failed sensor frames.
- One current reading per supported sensor is overwritten by newer observations.
- One exact wire body remains frozen only while its HTTPS attempt owns it.
- A send selects readings no older than 3,000,000 microseconds at the send boundary.
- Failed/unconfirmed readings are discarded, never counted as delivered. Replacements,
  expiry and unconfirmed counts are separate from exact server ACK counts.
- Acquisition/handoff keeps draining independently of TLS. Network readiness/backoff
  is checked before building a new body, so a snapshot does not age inside a retry queue.
- Boot metadata is attempted once after network readiness. Saved profiles/token and
  historical filesystem data are preserved; no new calibration or erase is performed.
- No lossless or power-loss durability claim is made. UI history contains only records
  actually accepted by the server, with no reconstruction of offline observations.
- The current prototype leaves processing to the separately labelled web TEST_ONLY
  analysis. The model descriptions below are legacy reference, not active MCU results.

## Firmware Build Identity

Latest-only identity is `sigap-live-https-v35`. The explicit legacy buffered prototype
(`SIGAP_LIVE_NO_BACKLOG=0`) retains `sigap-prototype-physical4-v27`.
Physical-reference and default identities remain unchanged outside prototype mode.

## Legacy buffered prototype reference (`SIGAP_LIVE_NO_BACKLOG=0`)

The retained model, commissioning and RAM-archive sections below describe the
inactive legacy path unless explicitly stated otherwise. They do not override
the latest-only policy above and are not physical acceptance evidence.

## Prototype Models

### FIS Model (Organic, 4-input, 16-rule Mamdani)

**Inputs:**
1. FillPercent (0..100%) - ultrasonic fill level
2. WeightUtilizationPercent (0..100%) - weight vs capacity
3. Ogi (0..100) - Organic Gas Index from MQ135 ADC
4. AccumulationHours (0..24h) - time since last empty

**Sets per input:** LOW (trapezoid [0,0,30,70]), HIGH (trapezoid [30,70,100,100])
**Output:** FIS_SCORE (0..100%) with LOW [0,0,20,45], MID triangle [20,50,80], HIGH [55,80,100,100]
**Rules:** 16 complete AND-MIN rules (all binary combinations)
**Consequent:** HIGH if FillHigh OR WeightHigh OR (OGIHigh AND HoursHigh); MID if OGIHigh OR HoursHigh; LOW otherwise
**Deployment:** TEST_ONLY, validity TestOnly

### Safety Model (Organic, temperature-only)

**Channel:** TemperatureC, required, alert >= 60°C, clear < 55°C, consecutive clear cycles = 2
**Deployment:** TEST_ONLY, validity TestOnly
**Behavior:** Non-actuating, missing input = unavailable (never fake CLEAR)

## Empty Reference Commissioning

On boot, captures up to 8 READY samples per sensor within 5s deadline:
- HX711: mean raw counts → tare_offset_counts
- Ultrasonic: mean echo time → empty distance (mm)

Does NOT produce fake zero on failure/timeout. Does NOT overwrite existing valid user calibration.

## Prototype Configuration (TEST_ONLY assumptions)

```cpp
struct PrototypeConfig {
  float hx711_counts_per_kg = 100000.0F;    // nominal gain
  float capacity_kg = 10.0F;                 // capacity assumption
  float ultrasonic_full_distance_mm = 20.0F; // full distance
  float sound_speed_mm_per_us = 0.1715F;     // 343 m/s at 20°C
  float adc_reference_mv = 3300.0F;          // ADC reference
  uint16_t adc_max_counts = 4095U;           // ADC resolution
  float adc_divider = 1.0F;                  // voltage divider
};
```

## OGI Computation

Average of available MQ ADC readings normalized to 0..100:
```
OGI = avg((mq7_mv / 3300) * 100, (mq8_mv / 3300) * 100)
```

## Accumulation Hours

Returns 0.0 when empty (fill ≤ 5% AND weight ≤ 0.05kg), unavailable otherwise. No persistent timer in prototype.

## HX711 Timing Fix

The HX711 reader uses a critical section (portENTER_CRITICAL_ISR) during clock pulses to prevent interrupt extension beyond the 50us HIGH max / 60us power-down threshold (AVIA datasheet p5). SCK is kept LOW on return.

## Network & ACK Flow

Prototype uses one network worker created in `setup()` from
`prototype_network_source_` (`ArchiveBatchNetworkSource<PrototypeLiveBatchRuntime>`).
The ACK is applied to the prototype batch runtime, which owns body freezing and
reclaim. Production keeps its own worker, `AsyncAckBatchSource` and filesystem
guard; nothing in this path changes them.

### Bounded multi-sample batching

`PrototypeLiveBatchRuntime::poll()` composes at most one batch per call from the
FIFO head of the volatile RAM archive:

- Consecutive `Sample` records that share one device identity and one boot
  identity are serialized into a single body. Grouping stops at the first record
  of a different kind, device or boot; that record is left in place for the next
  batch. Records are never dropped, reordered or blocked behind a head.
- A group is bounded by the frozen contract: at most 8 records per group in this
  prototype runtime, and never more than the contract's 128 records / 32 KiB of
  body. A record that would not fit ends the group instead of corrupting the
  body. The 8-record cap only bounds how many records one body can hold hostage;
  it reserves no store capacity. Composing a group removes nothing from the RAM
  store, and those records are reclaimed only after the ACK, so a store that is
  already at its 16-slot capacity has **zero** free slots while the batch is in
  flight. New arrivals then report `LoggingBlocked` and the handoff drop is
  counted, not hidden.
- Metadata, event and result records keep the existing one-record batch path and
  are never delayed by sample accumulation.
- A one-record group produces byte-identical output to
  `BatchJsonBuilder::build(single)`, so the existing single-record wire contract
  is preserved.
- The batch ID stays derived from the first record identity
  (`device/boot/sequence/batch`). Retries reuse the frozen body and ID.

### Bounded accumulation

Calling `poll()` every loop must not emit a one-record batch per loop. With a
bound clock (`bind_clock`) the runtime waits for either `kBatchTargetRecords`
(6) compatible records or `kBatchMaxWaitUs` (500 ms) since the head was first
seen, whichever comes first, then flushes what is available. A different record
behind the sample run, or a non-sample head, is flushed immediately. Sensor
cadence, drain budgets and drop counters are unchanged; nothing is masked or
delayed beyond this bounded window. Host fixtures that do not bind a clock keep
the immediate-build behaviour.

### Exact ACK and safe reclaim

The exact body, batch ID, body hash, record count and the full source manifest
(device, boot, sequence, kind and archive record hash for every record) are
frozen while HTTP is in flight. Nothing is rebuilt or re-selected before the ACK
decision.

An ACK is accepted only when it matches the frozen batch on device ID, batch ID,
body hash and record count, and `stored_count + deduplicated_count` equals the
record count. A partial or wrong ACK is rejected without reclaiming anything.

After a whole-batch ACK, every remaining source is prevalidated against the RAM
archive before the first reclaim. Sources are then reclaimed head-first in
manifest order. If a reclaim step fails, the completed prefix is retained, the
batch stays pending and the ACK retry resumes at the first unreclaimed source:
an already-retired prefix is never required again and an unexpected FIFO head is
never skipped.

A source that vanished or changed before the ACK is a real fault, and the
runtime fails closed. The ACK is reported as non-Durable (`Retryable`), and the
exact body, batch ID, body hash, record count, manifest and reclaim cursor are
retained unchanged: nothing is rebuilt, discarded, skipped or re-selected. A
retry of the same ACK keeps returning non-Durable, and `poll()` keeps reporting
the same frozen batch rather than composing a new one under the same
first-derived batch ID. This deliberately holds the pipeline until an operator
recovers it (restore the source, or rebind/restart the runtime); there is no
implicit abandonment, drop or recovery fallback. The fault is counted once per
in-flight batch in `source_mismatch`.

### Diagnostics: batches vs records

Sample grouping preserves archive append order, not global capture order.
The priority handoff can append distinct sequences out of order across channels.
As required by the telemetry-v3 contract (TSD section 8, array-order paragraph),
the bounded builder accepts those unique IDs without sorting or renumbering.
It rejects duplicates anywhere in the group, including changed-payload duplicates;
device/boot/kind boundaries and exact whole-batch ACK/reclaim remain unchanged.
The generic builder uses a fixed 128-entry identity array, with no allocation.
The application group cap remains eight. This source correction is not evidence
of improved physical throughput until a separately authorized installation.

`SIGAP_FLOW prototype` now reports both counters separately:

- `batches_built`, `batches_delivered` — whole batches composed and ACKed. Both
  include the one-record metadata/event/result batches, so `built` is never
  smaller than `delivered`.
- `records_delivered` — records retired across those batches.
- `network_delivered_batches` — network-level Delivered outcomes (one per batch).
- `batch_records`, `batch_reclaimed` — current in-flight manifest size and
  reclaim progress.
- `source_mismatch` — in-flight batches whose frozen manifest was rejected
  because a source is missing or changed. It is charged once per batch: a
  repeated ACK retry of the same fault does not increment it again.
- `partial_reclaim` — in-flight batches whose reclaim stopped after a completed
  prefix (progress > 0). A total reclaim failure with no progress is not a
  partial reclaim and does not increment it; repeated retries of the same
  partial failure keep it at one per batch.
- `ram_records`, `queued_records`, `handoff_dropped` — unchanged volatile
  pipeline counters.

The older `delivered` counter counted batches, not records; do not read it as a
record count.

## Volatile completion semantics and remaining limits

- The archive, batch runtime and ACK journal are RAM-only. ACK success means the
  server durably stored that exact batch body; it does **not** make the device
  archive power-loss durable, and it is not a lossless-delivery guarantee.
- The prototype pipeline is best-effort: acquisition handoff drops, RAM store
  capacity (16 records) and the bounded body/count limits still apply and are
  reported, not hidden.
- A batch that cannot be fully reclaimed stays frozen and pending; it is not
  discarded and no later record is reordered ahead of it.
- A frozen batch whose source vanished or changed stays frozen and pending too,
  under the fail-closed rule above. That is a deliberate stall, not a silent
  drop; only an operator recovery (restore the source, rebind or restart) clears
  it.
- The frozen group cap does not reserve RAM capacity, so a full 16-slot store
  during an in-flight batch blocks new appends rather than evicting anything.
- The historical physical baseline (60 s, 18 single-record HTTPS requests with
  matching ACK, 1229 handoff drops) is a previous observation, not proof of the
  current build. Record throughput and drop deltas must be measured again after
  a qualified build/flash at unchanged cadence.

## Usage

### Continuous acquisition and temporary SD disable

- Acquisition has no small cumulative sample limit. Scheduler job capacity is a
  registration bound, not a reading counter; target time and sequence use 64 bits.
- RAM capacity (16 records) and the group cap (eight) are bounded queues/bodies,
  not commands to stop sensors. A full pipeline can still lose new samples.
- The repository SD opt-in flags are temporarily commented out. The physical4
  target configuration already omits SD. Startup reports `SD_BACKUP=DISABLED`;
  saved NVS configuration/profiles and historical files are not erased.
- HTTPS completion publication and a contended mutex remain `Pending`. A worker
  can publish `Complete` before its semaphore token is visible; treating that
  interval as `Failed` loses the network worker's pending state and strands the
  transport outside `Idle`. Real failures and unstarted transports still fail.
- This source correction is not a physical diagnosis of an earlier stopped
  sensor. A fresh boot was observed during USB attachment; no reset command was
  sent, but attachment was not proven reset-free and the pre-stall state was lost.
- Keep host/native checks deferred where the owner has held them. The new
  completion probe is a source oracle until explicitly executed. A target build
  does not establish long-running acquisition or authorize a new flash.

```ini
# platformio.ini
build_flags = -DSIGAP_PROTOTYPE_OPERATION=1
```

**Constraints:**
- Cannot combine with `SIGAP_SENSOR_SIMULATION=1`
- Requires physical sensors
- Production mode and saved data are preserved
- No format/erase/old record deletion
- No bootloader/partition/token/TLS/firewall/heater/actuator changes