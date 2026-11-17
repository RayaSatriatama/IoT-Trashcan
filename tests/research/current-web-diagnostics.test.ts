import { describe, expect, it } from 'vitest';
import type { FirmwareMonitoringSample, SensorHistoryPoint } from '../../lib/api/types';
import { buildTestOnlyDiagnostics, deriveOgi } from '../../lib/ui/testOnlyDiagnostics';
import { arrivedSensorIds, describeLiveSample } from '../../lib/ui/liveSensorState';
// Unit fixtures only, never live sensor evidence.
const row = (sensor_id: FirmwareMonitoringSample['sensor_id'], seq: string, converted_value: Record<string, number>, extra: Partial<FirmwareMonitoringSample> = {}): FirmwareMonitoringSample => ({
  sensor_id, stream: 'organic', boot_id: 'current-boot', last_sequence: seq,
  quality: 'READY', reason: 'DATA_READY', sample_origin: 'UNKNOWN', origin_firmware_version: null,
  raw: null, converted_value, fresh: null, age_ms: 1000, ordering_unknown: true, future_clock_skew: false, ...extra,
});
const point = (seq: string, seconds: number, converted: Record<string, number>, extra: Partial<SensorHistoryPoint> = {}): SensorHistoryPoint => ({
  boot_id: 'current-boot', sequence: seq, monotonic_us: String(seconds * 1e6),
  recorded_at: new Date(Date.parse('2026-11-13T10:00:00Z') + seconds * 1000).toISOString(),
  time_quality: 'SYNCED', quality: 'READY', reason: 'DATA_READY', sample_origin: 'UNKNOWN', raw: null, converted, ...extra,
});
const inputs = () => ({
  monitoring: [row('hx711_organic', '2', { kg: 1 }), row('mq135', '3', { adc_mv: 1650 }), row('sht31_44', '4', { temperature_c: 30 }), row('ultrasonic_organic', '5', { distance_mm: 125.0765 })],
  history: { hx711_organic: [point('1', 0, { kg: 0 }), point('2', 3600, { kg: 1 })], mq135: [point('3', 3600, { adc_mv: 1650 })], sht31_44: [point('4', 3600, { temperature_c: 30 })] },
});
describe('current web diagnostics boundaries', () => {
  it('uses an explicit published zero fill without inventing a distance', () => {
    const input = inputs(); input.monitoring[3] = { ...input.monitoring[3], converted_value: { fill_percent: 0 } };
    const metric = buildTestOnlyDiagnostics(input).fillPercent;
    expect(metric.state).toBe('READY'); expect(metric.value).toBe(0);
    expect(describeLiveSample(input.monitoring[3]).values.some(value => value.text === '0 %')).toBe(true);
  });
  it('does not mask an invalid published fill with a geometry fallback', () => {
    const input = inputs(); input.monitoring[3] = { ...input.monitoring[3], converted_value: { fill_percent: -1, distance_mm: 20 } };
    expect(buildTestOnlyDiagnostics(input).fillPercent.value).toBeNull();
  });
  it('derives fuzzy inputs from current sensors and declared TEST_ONLY geometry, without MCU results', () => {
    const result = buildTestOnlyDiagnostics(inputs());
    expect(result.fuzzy.state).toBe('READY');
    expect(result.fuzzy.inputs.find(x => x.input === 'fill_pct')?.value).toBeCloseTo(50, 3);
    expect(result.fuzzy.inputs.find(x => x.input === 'weight_utilization_pct')?.value).toBe(10);
    expect(result.ogi.origin).toBe('UNKNOWN');
  });
  it('rejects old capture ages even when the API freshness policy is null', () => {
    const input = inputs(); input.monitoring = input.monitoring.map(x => ({ ...x, age_ms: 35_000 }));
    const result = buildTestOnlyDiagnostics(input);
    expect(result.ogi.value).toBeNull(); expect(result.ogi.reason).toBe('INPUT_STALE');
    expect(result.accumulationHours.value).toBeNull(); expect(result.fuzzy.score).toBeNull();
  });
  it('refuses historical data from another boot', () => {
    const input = inputs(); input.history.mq135 = [point('3', 3600, { adc_mv: 1650 }, { boot_id: 'old-boot' })];
    expect(buildTestOnlyDiagnostics(input).ogi.value).toBeNull();
  });
  it('requires an owned current snapshot rather than treating orphan history as live', () => {
    expect(buildTestOnlyDiagnostics({ history: inputs().history }).accumulationHours.value).toBeNull();
  });
  it('does not mask an invalid newest gas reading with an older valid reading', () => {
    expect(deriveOgi([point('1', 1, { adc_mv: 1650 }), point('2', 2, { adc_mv: 0 }, { quality: 'STALE' })]).value).toBeNull();
  });
  it('rejects a future-clock sample for current analysis', () => {
    const input = inputs(); input.monitoring[1] = { ...input.monitoring[1], future_clock_skew: true };
    expect(buildTestOnlyDiagnostics(input).ogi.value).toBeNull();
  });
  it('rejects a history endpoint ahead of the published current sequence', () => {
    const input = inputs(); input.history.mq135 = [point('99', 3600, { adc_mv: 1650 })];
    expect(buildTestOnlyDiagnostics(input).ogi.value).toBeNull();
  });
});
describe('live display recency boundaries', () => {
  it('ages a held response locally without changing its source object', () => {
    const sample = row('mq135', '3', { adc_mv: 0 }, { age_ms: 1000 });
    expect(describeLiveSample(sample, 35_000).recencyLabel).toBe('Tertunda (>30 detik)');
    expect(sample.age_ms).toBe(1000);
  });
  it('does not announce an older same-boot sequence as a new arrival', () => {
    const old = row('mq135', '20', { adc_mv: 0 });
    expect(arrivedSensorIds(new Map([['mq135', old]]), [row('mq135', '19', { adc_mv: 0 })])).toEqual([]);
  });
});
