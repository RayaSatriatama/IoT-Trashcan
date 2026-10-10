-- Canonical authenticated BLE identity provisioned from the board EFUSE ID.
-- Legacy device_code remains the operator/archive identifier; a missing value
-- keeps local BLE calibration unavailable rather than guessing an identity.
ALTER TABLE devices ADD COLUMN ble_device_id TEXT;

CREATE UNIQUE INDEX idx_devices_ble_device_id
  ON devices(ble_device_id)
  WHERE ble_device_id IS NOT NULL;
