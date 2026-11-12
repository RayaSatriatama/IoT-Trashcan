CREATE TABLE device_debug_history (
    ingestion_id INTEGER PRIMARY KEY AUTOINCREMENT CHECK (ingestion_id > 0),
    device_code TEXT NOT NULL REFERENCES devices(device_code) ON DELETE CASCADE,
    boot_id TEXT NOT NULL CHECK (length(boot_id) = 32),
    sequence_text TEXT NOT NULL,
    event_json TEXT NOT NULL,
    build_id TEXT NOT NULL,
    received_at TEXT NOT NULL,
    snapshot_level TEXT NOT NULL,
    captured_monotonic_us TEXT NOT NULL,
    oldest_sequence TEXT NOT NULL,
    overwritten TEXT NOT NULL,
    delivery_lost TEXT NOT NULL,
    UNIQUE (device_code, boot_id, sequence_text)
);

CREATE INDEX idx_device_debug_history_page
  ON device_debug_history(device_code, ingestion_id);
CREATE INDEX idx_device_debug_history_received
  ON device_debug_history(received_at, ingestion_id);

CREATE TABLE device_debug_watermarks (
    device_code TEXT PRIMARY KEY REFERENCES devices(device_code) ON DELETE CASCADE,
    pruned_through INTEGER NOT NULL CHECK (pruned_through > 0)
);
