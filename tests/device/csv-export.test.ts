import { describe, expect, it } from 'vitest';
import { buildHistoryCsv, csvFilename } from '../../lib/ui/csvExport';
import type { SensorHistoryPoint } from '../../lib/api/types';

const point = (overrides: Partial<SensorHistoryPoint> = {}): SensorHistoryPoint => ({
  boot_id: 'boot-a', sequence: '1', monotonic_us: '10', recorded_at: '2026-11-16T01:00:00Z',
  time_quality: 'SYNCED', quality: 'READY', reason: 'DATA_READY', sample_origin: 'PHYSICAL',
  raw: {}, converted: {}, ...overrides,
});

describe('history CSV export', () => {
  it('writes one header and one row per sample with the sensor metric columns', () => {
    const csv = buildHistoryCsv({
      hx711_organic: [point({ converted: { kg: 0.18466 } })],
      mq135: [point({ converted: { adc_mv: 2831, adc_pin_mv: 2831, sensor_ao_mv: 2831, adc_efuse_calibrated: 1 } })],
    });
    const lines = csv.trim().split('\n');
    expect(lines).toHaveLength(3);
    expect(lines[0]).toContain('sensor_id,recorded_at,time_quality,quality');
    expect(lines[0]).toContain('kg (kg)');
    expect(lines[0]).toContain('adc_pin_mv (mV)');
    expect(lines[0]).toContain('nh3_ppm (ppm)');
    expect(lines[1]).toContain('hx711_organic');
    expect(lines[1]).toContain('0.18466');
    expect(lines[2]).toContain('mq135');
    expect(lines[2]).toContain('2831');
  });

  it('leaves cells empty instead of inventing values for other sensors', () => {
    const csv = buildHistoryCsv({
      hx711_organic: [point({ converted: { kg: 0.5 } })],
      sht31_44: [point({ converted: { temperature_c: 31.9, humidity_percent: 65.9 } })],
    });
    const row = csv.trim().split('\n')[1];
    expect(row).toContain('hx711_organic');
    // The sht31 columns exist but stay blank on the weight row.
    const cells = row.split(',');
    expect(cells.filter(value => value === '').length).toBeGreaterThanOrEqual(2);
  });

  it('escapes separators and quotes in text cells', () => {
    const csv = buildHistoryCsv({ mq135: [point({ reason: 'A,B"C' })] });
    expect(csv).toContain('"A,B""C"');
  });

  it('includes the NH3 estimate for MQ135 rows and leaves it blank elsewhere', () => {
    const csv = buildHistoryCsv({
      hx711_organic: [point({ converted: { kg: 1 } })],
      mq135: [point({ converted: { adc_pin_mv: 2831 } })],
    });
    const rows = csv.trim().split('\n');
    const header = rows[0].split(',');
    const ppmIndex = header.indexOf('nh3_ppm (ppm)');
    const statusIndex = header.indexOf('nh3_status');
    expect(ppmIndex).toBeGreaterThan(-1);
    const gasRow = rows.find(row => row.startsWith('mq135'))!.split(',');
    expect(Number.parseFloat(gasRow[ppmIndex])).toBeGreaterThan(0);
    expect(gasRow[statusIndex]).toMatch(/^(IN_RANGE|EXTRAPOLATED)$/);
    const weightRow = rows.find(row => row.startsWith('hx711_organic'))!.split(',');
    expect(weightRow[ppmIndex]).toBe('');
    expect(weightRow[statusIndex]).toBe('');
  });

  it('keeps raw fields so a sample without a converted value still exports', () => {
    const csv = buildHistoryCsv({
      ultrasonic_organic: [point({ raw: { echo_us: 1098 }, converted: null })],
    });
    const lines = csv.trim().split('\n');
    expect(lines[0]).toContain('raw_echo_us');
    const cells = lines[1].split(',');
    expect(cells[0]).toBe('ultrasonic_organic');
    expect(cells[cells.length - 1]).toBe('1098');
  });

  it('names the file after the device and timestamp', () => {
    const name = csvFilename('dev-real', new Date('2026-11-16T02:03:04Z'));
    expect(name).toBe('sigap-dev-real-2026-11-16T02-03-04.csv');
    expect(csvFilename('a/b c')).toMatch(/^sigap-a_b_c-/);
  });
});
