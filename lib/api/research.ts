import type { ResearchResponse } from './types';
import type { CalibrationSensorId, SensorDiagnostic } from './calibration';

function sensorStream(sensorId: string): string {
  if (sensorId.endsWith('_inorganic')) return 'inorganic';
  if (sensorId.endsWith('_b3') || sensorId === 'mq8') return 'b3';
  return 'organic';
}

export function diagnosticFromResearch(sensorId: CalibrationSensorId, research: ResearchResponse | null | undefined): SensorDiagnostic | null {
  const sensor = research?.slots.flatMap((slot) => slot.sensors ?? []).find((entry) => entry.sensor_id === sensorId);
  if (!sensor) return null;
  const quality = sensor.sensor_state === 'VALID' ? 'READY' : sensor.sensor_state === 'WARMING_UP' ? 'CALIBRATING' : sensor.sensor_state === 'STALE' ? 'STALE' : 'INVALID';
  return {
    sensor_id: sensorId,
    stream: sensorStream(sensorId),
    quality,
    reason: quality === 'READY' ? 'DATA_READY' : quality,
    raw: { present: sensor.raw_value !== null, value: sensor.raw_value, unit: sensor.raw_unit },
    converted_value: sensor.engineering_value,
    converted_unit: sensor.unit || null,
    fresh: quality !== 'STALE' && quality !== 'INVALID',
    age_ms: null,
    last_sequence: null,
    profile_present: Boolean(sensor.calibration_version),
    profile_version: sensor.calibration_version ?? null,
    profile_hash: null,
    calibration_state: 'IDLE',
    accepted_samples: 0,
    rejected_samples: 0,
  };
}
