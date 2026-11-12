import { describe, expect, it } from 'vitest';
import { CALIBRATION_SENSOR_IDS } from '../../lib/api/calibration';
import { calibrationSensorLabel, CALIBRATION_STEPS, WIFI_STEPS } from '../../lib/ui/deviceTools';

describe('UI Kit V2.3 operational presentation', () => {
  it('labels every canonical sensor without changing the routing values', () => {
    const labels = CALIBRATION_SENSOR_IDS.map(calibrationSensorLabel);
    expect(new Set(labels).size).toBe(CALIBRATION_SENSOR_IDS.length);
    expect(labels.every(label => !label.includes('_'))).toBe(true);
    expect(calibrationSensorLabel('hx711_organic')).toBe('Berat · Organik (HX711)');
    expect(calibrationSensorLabel('sht31_44')).toContain('0x44');
  });
  it('keeps seven actual calibration phases and three Wi-Fi admission phases', () => {
    expect(CALIBRATION_STEPS.map(step => step.id)).toEqual(['diagnostics','criteria','capture','validate','preview','commit','done']);
    expect(WIFI_STEPS.map(step => step.id)).toEqual(['connect','identity','network']);
    expect([...CALIBRATION_STEPS, ...WIFI_STEPS].every(step => !!step.description)).toBe(true);
  });
});
