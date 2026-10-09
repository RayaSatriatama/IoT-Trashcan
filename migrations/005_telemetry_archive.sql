-- Durable telemetry-v3 archive.  The request body is retained byte-for-byte so
-- retries and exports never rebuild a payload through a different serializer.
CREATE TABLE telemetry_batches (
    device_id TEXT NOT NULL,
    batch_id TEXT NOT NULL,
    body_hash TEXT NOT NULL,
    body_bytes BLOB NOT NULL,
    record_count INTEGER NOT NULL CHECK (record_count >= 0 AND record_count <= 128),
    stored_count INTEGER NOT NULL CHECK (stored_count >= 0 AND stored_count <= record_count),
    deduplicated_count INTEGER NOT NULL CHECK (deduplicated_count >= 0 AND deduplicated_count <= record_count),
    status TEXT NOT NULL CHECK (status = 'DURABLE'),
    created_at TEXT NOT NULL,
    acked_at TEXT,
    FOREIGN KEY (device_id) REFERENCES devices(device_code),
    PRIMARY KEY (device_id, batch_id),
    UNIQUE (device_id, batch_id, body_hash)
);

CREATE TABLE telemetry_records (
    device_id TEXT NOT NULL,
    record_id TEXT NOT NULL,
    record_hash TEXT NOT NULL,
    kind TEXT NOT NULL CHECK (kind IN ('sample', 'result', 'event', 'metadata')),
    boot_id TEXT NOT NULL,
    sequence_decimal TEXT NOT NULL,
    sequence_sort TEXT NOT NULL CHECK (length(sequence_sort) = 20),
    monotonic_us TEXT NOT NULL,
    recorded_at TEXT,
    time_quality TEXT NOT NULL,
    stream TEXT,
    sensor_id TEXT,
    quality TEXT,
    record_bytes BLOB NOT NULL,
    first_batch_id TEXT NOT NULL,
    created_at TEXT NOT NULL,
    FOREIGN KEY (device_id) REFERENCES devices(device_code),
    PRIMARY KEY (device_id, record_id),
    UNIQUE (device_id, record_id, record_hash)
);

CREATE TABLE telemetry_batch_records (
    device_id TEXT NOT NULL,
    batch_id TEXT NOT NULL,
    record_id TEXT NOT NULL,
    record_hash TEXT NOT NULL,
    PRIMARY KEY (device_id, batch_id, record_id),
    FOREIGN KEY (device_id, batch_id) REFERENCES telemetry_batches(device_id, batch_id),
    FOREIGN KEY (device_id, record_id, record_hash)
        REFERENCES telemetry_records(device_id, record_id, record_hash)
);

CREATE INDEX idx_telemetry_batches_created
    ON telemetry_batches(device_id, created_at DESC);
CREATE INDEX idx_telemetry_records_created
    ON telemetry_records(device_id, created_at DESC);
CREATE INDEX idx_telemetry_records_cursor
    ON telemetry_records(device_id, boot_id, sequence_sort, record_id);
CREATE INDEX idx_telemetry_records_source
    ON telemetry_records(device_id, kind, stream, sensor_id, quality);

-- One durable candidate per source and boot keeps current-state reads bounded
-- by the number of boots/sources instead of the total retained record count.
-- The complete record archive remains authoritative; this table is only a
-- replaceable read projection and retains the original record bytes.
CREATE TABLE telemetry_current_candidates (
    device_id TEXT NOT NULL,
    kind TEXT NOT NULL CHECK (kind IN ('sample', 'result')),
    stream TEXT NOT NULL,
    sensor_id TEXT NOT NULL DEFAULT '',
    boot_id TEXT NOT NULL,
    record_id TEXT NOT NULL,
    record_hash TEXT NOT NULL,
    sequence_sort TEXT NOT NULL CHECK (length(sequence_sort) = 20),
    monotonic_us TEXT NOT NULL,
    recorded_at TEXT,
    time_quality TEXT NOT NULL,
    record_bytes BLOB NOT NULL,
    first_record_rowid INTEGER NOT NULL,
    PRIMARY KEY (device_id, kind, stream, sensor_id, boot_id)
);

CREATE INDEX idx_telemetry_current_source
    ON telemetry_current_candidates(device_id, kind, stream, sensor_id,
                                    first_record_rowid);

CREATE TRIGGER telemetry_records_current_candidate
AFTER INSERT ON telemetry_records
WHEN NEW.kind IN ('sample', 'result') AND NEW.stream IS NOT NULL
BEGIN
    INSERT INTO telemetry_current_candidates
      (device_id, kind, stream, sensor_id, boot_id, record_id, record_hash,
       sequence_sort, monotonic_us, recorded_at, time_quality, record_bytes,
       first_record_rowid)
    VALUES
      (NEW.device_id, NEW.kind, NEW.stream, COALESCE(NEW.sensor_id, ''),
       NEW.boot_id, NEW.record_id, NEW.record_hash, NEW.sequence_sort,
       NEW.monotonic_us, NEW.recorded_at, NEW.time_quality, NEW.record_bytes,
       NEW.rowid)
    ON CONFLICT(device_id, kind, stream, sensor_id, boot_id) DO UPDATE SET
       record_id = excluded.record_id,
       record_hash = excluded.record_hash,
       sequence_sort = excluded.sequence_sort,
       monotonic_us = excluded.monotonic_us,
       recorded_at = excluded.recorded_at,
       time_quality = excluded.time_quality,
       record_bytes = excluded.record_bytes
    WHERE excluded.sequence_sort > telemetry_current_candidates.sequence_sort
       OR (excluded.sequence_sort = telemetry_current_candidates.sequence_sort
           AND excluded.monotonic_us > telemetry_current_candidates.monotonic_us)
       OR (excluded.sequence_sort = telemetry_current_candidates.sequence_sort
           AND excluded.monotonic_us = telemetry_current_candidates.monotonic_us
           AND excluded.record_id > telemetry_current_candidates.record_id);
END;
