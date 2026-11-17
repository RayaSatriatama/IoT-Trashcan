import { describe, expect, it } from 'vitest';

import type { FirmwareMonitoringSample } from '../../lib/api/types';
import { sensorValueRows } from '../../lib/ui/firmwarePresentation';
import { estimateMq135Ppm } from '../../lib/ui/testOnlyDiagnostics';
import {
  RECENT_SAMPLE_MS,
  arrivedSensorIds,
  captureIdentity,
  describeLiveSample,
  formatSampleAge,
} from '../../lib/ui/liveSensorState';

// Fixture-only shapes copied from the documented telemetry-v3 monitoring projection.
// They are not physical-device evidence.
const sample = (overrides: Partial<FirmwareMonitoringSample> = {}): FirmwareMonitoringSample => ({
  sensor_id: 'sht31_44', stream: 'organic', boot_id: 'boot-a', last_sequence: '9007199254740993',
  quality: 'READY', reason: 'DATA_READY', sample_origin: 'UNKNOWN', origin_firmware_version: null,
  raw: { temperature_ticks: 0, humidity_ticks: 0 }, converted_value: { temperature_c: 0, humidity_percent: 0 },
  fresh: null, age_ms: 2_786_083, ordering_unknown: true, future_clock_skew: false, ...overrides,
});

describe('live sensor sample state', () => {
  it('calculates reference ppm from changing actual ADC input, not a fixed ppm output', () => {
    expect(estimateMq135Ppm(2500)).toBeCloseTo(32782.14969764746, 5);
    expect(estimateMq135Ppm(3000)!).toBeGreaterThan(estimateMq135Ppm(2500)!);
    for (const invalid of [null, Number.NaN, 0, 5000]) expect(estimateMq135Ppm(invalid)).toBeNull();
  });
  it('removes only the weight display sign while preserving source data and signed temperature', () => {
    const input = sample({ sensor_id: 'hx711_organic', converted_value: { kg: -0.42649 }, raw: { counts: -1234 } });
    expect(describeLiveSample(input).values).toEqual([{ label: 'Berat', text: '0.42649 kg' }]);
    expect(sensorValueRows('hx711_organic', input.converted_value, input.raw)).toEqual([['Massa', '0.42649 kg'], ['Hitungan HX711', '-1234 counts']]);
    expect(input.converted_value?.kg).toBe(-0.42649);
    expect(describeLiveSample(sample({ converted_value: { temperature_c: -5, humidity_percent: 60 } })).values[0].text).toBe('-5 °C');
  });
  it('keeps READY with an unset freshness policy distinct from fresh and stale', () => {
    const view = describeLiveSample(sample());
    expect(view.state).toBe('VALID');
    expect(view.freshnessLabel).toBe('Kesegaran belum ditetapkan API');
    expect(view.freshnessLabel).not.toContain('Segar');
    expect(view.freshnessLabel).not.toContain('Tidak segar');
  });

  it('reports the API freshness value without inventing one', () => {
    expect(describeLiveSample(sample({ fresh: true })).freshnessLabel).toBe('Segar menurut API');
    expect(describeLiveSample(sample({ fresh: false })).freshnessLabel).toBe('Tidak segar menurut API');
  });

  it('reports the observed sample age and a UI-only recency category', () => {
    const view = describeLiveSample(sample({ age_ms: 2_786_083 }));
    expect(view.ageLabel).toBe('Umur sampel (estimasi): 46 menit');
    expect(view.recencyLabel).toBe('Tertunda (>30 detik)');
    expect(describeLiveSample(sample({ age_ms: 9_500 })).recencyLabel).toBe('Terbaru (≤30 detik)');
    expect(RECENT_SAMPLE_MS).toBe(30_000);
  });

  it('treats a future clock as an untrustworthy age instead of a fresh sample', () => {
    const view = describeLiveSample(sample({ future_clock_skew: true, age_ms: 0 }));
    expect(view.ageLabel).toContain('jam contoh di masa depan');
    expect(view.recencyLabel).toBeNull();
    expect(view.freshnessLabel).toBe('Kesegaran belum ditetapkan API');
  });

  it('does not trust malformed or negative ages', () => {
    for (const age of [null, -5, Number.NaN, Number.POSITIVE_INFINITY]) {
      const view = describeLiveSample(sample({ age_ms: age as number | null }));
      expect(view.ageLabel).toBe('Umur contoh tidak diketahui');
      expect(view.recencyLabel).toBeNull();
    }
    expect(formatSampleAge(null)).toBe('tidak diketahui');
    expect(formatSampleAge(1_200)).toBe('1 detik');
    expect(formatSampleAge(90_000)).toBe('1 menit');
    expect(formatSampleAge(7_200_000)).toBe('2 jam');
    expect(formatSampleAge(172_800_000)).toBe('2 hari');
  });

  it('keeps cross-boot ordering uncertainty separate from sample freshness', () => {
    const view = describeLiveSample(sample({ ordering_unknown: true }));
    expect(view.orderingNote).toBe('Urutan lintas boot tidak diketahui');
    expect(view.freshnessLabel).toBe('Kesegaran belum ditetapkan API');
    expect(describeLiveSample(sample({ ordering_unknown: false })).orderingNote).toBeNull();
  });

  it('exposes only converted engineering values and preserves a real zero', () => {
    const view = describeLiveSample(sample({ converted_value: { temperature_c: 0, humidity_percent: 0 } }));
    expect(view.values).toEqual([{ label: 'Suhu', text: '0 °C' }, { label: 'Kelembapan', text: '0 %RH' }]);
    const gas = describeLiveSample(sample({ sensor_id: 'mq135', converted_value: { adc_mv: 2500 } }));
    expect(gas.values).toEqual([{ label: 'Tegangan AO', text: '2500 mV' }]);
    expect(gas.values.some(row=>row.text.includes('ppm'))).toBe(false);
    expect(gas.values[0].text).not.toContain('2500 ppm');
  });

  it('never presents converted values for a non-READY sample', () => {
    const view = describeLiveSample(sample({ quality: 'UNCALIBRATED', reason: 'INVALID_PROFILE' }));
    expect(view.state).toBe('INVALID');
    expect(view.values).toEqual([{ label: 'Suhu', text: 'Belum tersedia' }, { label: 'Kelembapan', text: 'Belum tersedia' }]);
    expect(view.reasonLabel).toBe('Profil kalibrasi belum valid');
  });

  it('labels origin without promoting unknown metadata to physical', () => {
    expect(describeLiveSample(sample({ sample_origin: 'PHYSICAL' })).originLabel).toBe('Fisik (metadata firmware)');
    expect(describeLiveSample(sample({ sample_origin: 'SIMULATED' })).originLabel).toBe('Simulasi firmware');
    expect(describeLiveSample(sample({ sample_origin: 'UNKNOWN' })).originLabel).toBe('Asal tidak diketahui');
  });

  it('builds capture identity from boot and sequence only', () => {
    const first = sample({ age_ms: 10_000 });
    const later = sample({ age_ms: 40_000 });
    expect(captureIdentity(first)).toBe(captureIdentity(later));
    expect(captureIdentity(sample({ last_sequence: '2' }))).not.toBe(captureIdentity(first));
    expect(captureIdentity(sample({ boot_id: 'boot-b' }))).not.toBe(captureIdentity(first));
  });

  it('does not animate the first observation or an unchanged poll', () => {
    const prior = new Map([[sample().sensor_id, sample()]]);
    expect(arrivedSensorIds(new Map(), [sample()])).toEqual([]);
    expect(arrivedSensorIds(prior, [sample({ age_ms: 99_999 })])).toEqual([]);
  });

  it('animates only a new recent READY capture', () => {
    const prior = new Map([[sample().sensor_id, sample()]]);
    expect(arrivedSensorIds(prior, [sample({ last_sequence: '9007199254740994', age_ms: 1_200 })])).toEqual(['sht31_44']);
    expect(arrivedSensorIds(prior, [sample({ boot_id: 'boot-b', age_ms: 900 })])).toEqual(['sht31_44']);
  });

  it('refuses arrival animation for stale, skewed, uncalibrated, or not-fresh samples', () => {
    const prior = new Map([[sample().sensor_id, sample()]]);
    const next = (overrides: Partial<FirmwareMonitoringSample>) => [sample({ last_sequence: '7', age_ms: 1_000, ...overrides })];
    expect(arrivedSensorIds(prior, next({ age_ms: 31_000 }))).toEqual([]);
    expect(arrivedSensorIds(prior, next({ future_clock_skew: true }))).toEqual([]);
    expect(arrivedSensorIds(prior, next({ quality: 'UNCALIBRATED' }))).toEqual([]);
    expect(arrivedSensorIds(prior, next({ fresh: false }))).toEqual([]);
    expect(arrivedSensorIds(prior, next({ age_ms: null }))).toEqual([]);
  });
});
