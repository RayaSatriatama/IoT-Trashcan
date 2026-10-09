-- Durable, device-bound calibration command delivery.  Command bodies are
-- retained exactly as submitted; result bodies are bounded and auditable.
CREATE TABLE device_control_commands (
    command_id TEXT PRIMARY KEY,
    device_id TEXT NOT NULL,
    operator_id TEXT NOT NULL,
    session_id TEXT,
    sensor_id TEXT NOT NULL,
    operation TEXT NOT NULL,
    payload BLOB NOT NULL,
    request_hash TEXT NOT NULL,
    status TEXT NOT NULL CHECK (status IN
      ('QUEUED','DELIVERED','ACCEPTED','CAPTURING','VALIDATED',
       'PREVIEW_READY','COMMITTED','FAILED','EXPIRED','CANCELLED')),
    expires_at TEXT NOT NULL,
    lease_until TEXT,
    attempt INTEGER NOT NULL DEFAULT 0 CHECK (attempt >= 0),
    result_payload BLOB,
    result_hash TEXT,
    error_code TEXT,
    created_at TEXT NOT NULL,
    updated_at TEXT NOT NULL,
    UNIQUE (device_id, command_id, request_hash),
    FOREIGN KEY (device_id) REFERENCES devices(device_code)
);

CREATE INDEX idx_device_control_delivery
  ON device_control_commands(device_id, status, expires_at, created_at);
CREATE INDEX idx_device_control_session
  ON device_control_commands(device_id, session_id, created_at);

CREATE TABLE device_control_sessions (
    device_id TEXT PRIMARY KEY,
    session_id TEXT NOT NULL,
    operator_id TEXT NOT NULL,
    sensor_id TEXT NOT NULL,
    expires_at TEXT NOT NULL,
    state TEXT NOT NULL CHECK (state IN ('ACTIVE','CLOSED')),
    created_at TEXT NOT NULL,
    updated_at TEXT NOT NULL,
    FOREIGN KEY (device_id) REFERENCES devices(device_code)
);
