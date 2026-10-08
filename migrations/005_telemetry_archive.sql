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
