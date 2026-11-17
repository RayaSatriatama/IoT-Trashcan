import { describe, expect, it } from 'vitest';
import { MonitoringAgeTracker } from '../../lib/ui/liveSensorState';
import type { FirmwareMonitoringSample } from '../../lib/api/types';
const sample = (extra: Partial<FirmwareMonitoringSample> = {}): FirmwareMonitoringSample => ({
  sensor_id: 'mq135', stream: 'organic', boot_id: 'unit-boot', last_sequence: '1',
  quality: 'READY', reason: 'DATA_READY', sample_origin: 'UNKNOWN', origin_firmware_version: null,
  raw: null, converted_value: { adc_mv: 0 }, fresh: null, age_ms: 1000,
  ordering_unknown: true, future_clock_skew: false, ...extra,
});
describe('UI monotonic sample age tracker', () => {
  it('does not reset an unchanged cached capture age on another response', () => {
    const tracker = new MonitoringAgeTracker(); const input = sample();
    expect(tracker.observe([input], 0)[0].age_ms).toBe(1000);
    expect(tracker.observe([input], 20_000)[0].age_ms).toBe(21_000);
    expect(tracker.observe([sample()], 40_000)[0].age_ms).toBe(41_000);
    expect(input.age_ms).toBe(1000);
  });
  it('accepts a conservatively older server estimate for the same capture', () => {
    const tracker = new MonitoringAgeTracker(); tracker.observe([sample()], 0);
    expect(tracker.observe([sample({ age_ms: 50_000 })], 20_000)[0].age_ms).toBe(50_000);
  });
  it('re-baselines only a new capture and preserves API policy', () => {
    const tracker = new MonitoringAgeTracker(); tracker.observe([sample()], 0);
    const next = tracker.observe([sample({ last_sequence: '2', age_ms: 0 })], 40_000)[0];
    expect(next.age_ms).toBe(0); expect(next.fresh).toBeNull();
  });
  it('does not turn an invalid age into a known age', () => {
    const tracker = new MonitoringAgeTracker(); tracker.observe([sample()], 0);
    expect(tracker.observe([sample({ age_ms: null })], 40_000)[0].age_ms).toBeNull();
  });
  it('clears previous scope and removes absent sensors', () => {
    const tracker = new MonitoringAgeTracker(); tracker.observe([sample()], 0);
    tracker.clear(); expect(tracker.observe([sample()], 40_000)[0].age_ms).toBe(1000);
    tracker.observe([], 50_000); expect(tracker.observe([sample()], 60_000)[0].age_ms).toBe(1000);
  });
});
