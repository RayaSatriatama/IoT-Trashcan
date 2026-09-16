CREATE TABLE locations (
    id TEXT PRIMARY KEY,
    name TEXT NOT NULL,
    address_text TEXT,
    active INTEGER NOT NULL DEFAULT 1 CHECK (active IN (0, 1))
);

CREATE TABLE devices (
    id TEXT PRIMARY KEY,
    location_id TEXT NOT NULL REFERENCES locations(id),
    device_code TEXT NOT NULL UNIQUE,
    placement_label TEXT,
    online_state TEXT,
    last_seen TEXT,
    firmware_version TEXT,
    credential_digest TEXT NOT NULL
);

CREATE TABLE slots (
    id TEXT PRIMARY KEY,
    device_id TEXT NOT NULL REFERENCES devices(id),
    waste_stream TEXT NOT NULL CHECK (waste_stream IN ('organic', 'inorganic', 'b3')),
    capacity REAL,
    UNIQUE (device_id, waste_stream)
);

CREATE TABLE readings (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    slot_id TEXT NOT NULL REFERENCES slots(id),
    recorded_at TEXT NOT NULL,
    raw_payload TEXT NOT NULL,
    fill_pct REAL,
    weight_kg REAL,
    ogi REAL,
    accumulated_mass REAL,
    temperature_c REAL,
    sensor_health TEXT
);

CREATE TABLE decisions (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    device_id TEXT NOT NULL REFERENCES devices(id),
    created_at TEXT NOT NULL,
    status TEXT NOT NULL,
    fis_version TEXT,
    detail TEXT
);

CREATE TABLE events (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    device_id TEXT NOT NULL REFERENCES devices(id),
    decision_id INTEGER REFERENCES decisions(id),
    event_type TEXT NOT NULL,
    severity TEXT,
    started_at TEXT NOT NULL,
    ended_at TEXT,
    detail TEXT
);

CREATE INDEX idx_devices_location ON devices(location_id);
CREATE INDEX idx_devices_last_seen ON devices(last_seen);
CREATE INDEX idx_readings_slot_recorded ON readings(slot_id, recorded_at);
CREATE INDEX idx_events_started ON events(started_at);
CREATE INDEX idx_events_device_started ON events(device_id, started_at);
CREATE INDEX idx_events_severity_started ON events(severity, started_at);
