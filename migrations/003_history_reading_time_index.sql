CREATE INDEX IF NOT EXISTS idx_readings_recorded_id
ON readings(recorded_at DESC, id DESC);
