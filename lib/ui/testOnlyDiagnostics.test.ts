import { describe, expect, it } from 'vitest';
import type { FirmwareMonitoringSample, FirmwareResult, SensorHistoryPoint } from '../api/types';
import {
  TEST_ONLY_PROFILE,
  buildTestOnlyDiagnostics,
  deriveAccumulationHours,
  deriveOgi,
  evaluateOrganicFis,
  evaluateSafetyDiagnostic,
} from './testOnlyDiagnostics';

const point = (overrides: Partial<SensorHistoryPoint> = {}): SensorHistoryPoint => ({
  boot_id: 'boot-a', sequence: '1', monotonic_us: '0', recorded_at: '2026-11-13T10:00:00Z',
  time_quality: 'SYNCED', quality: 'READY', reason: 'READY', sample_origin: 'PHYSICAL',
  raw: null, converted: null, ...overrides,
});

const weightPoint = (kg: number, seconds: number, overrides: Partial<SensorHistoryPoint> = {}) => point({
  sequence: String(seconds + 1), monotonic_us: String(seconds * 1_000_000),
  recorded_at: new Date(Date.parse('2026-11-13T10:00:00Z') + seconds * 1000).toISOString(),
  converted: { kg }, ...overrides,
});

const gasPoint = (adcMv: number, overrides: Partial<SensorHistoryPoint> = {}) => point({ converted: { adc_mv: adcMv }, ...overrides });

const tempPoint = (celsius: number, seconds: number, overrides: Partial<SensorHistoryPoint> = {}) => point({
  sequence: String(seconds + 1), monotonic_us: String(seconds * 1_000_000),
  converted: { temperature_c: celsius, humidity_percent: 60 }, ...overrides,
});

const monitoring = (overrides: Partial<FirmwareMonitoringSample> = {}): FirmwareMonitoringSample => ({
  sensor_id: 'mq135', stream: 'organic', boot_id: 'boot-a', last_sequence: '9', quality: 'READY',
  reason: 'READY', sample_origin: 'PHYSICAL', origin_firmware_version: null,
  raw: { adc_counts: 2048 }, converted_value: { adc_mv: 1650 }, fresh: true, age_ms: 500,
  ordering_unknown: false, future_clock_skew: false, ...overrides,
});

const result = (processed: Partial<FirmwareResult['processed']> = {}): FirmwareResult => ({
  stream: 'organic', boot_id: 'boot-a', sequence: '9', monotonic_us: '9000000',
  recorded_at: '2026-11-13T10:00:00Z', time_quality: 'SYNCED', source_sample_ids: ['s'],
  result_origin: 'PHYSICAL', origin_firmware_version: null, fresh: true, age_ms: 500,
  future_clock_skew: false, ordering_unknown: false,
  processed: { fill_pct: null, weight_kg: null, weight_utilization_pct: null, ogi: null, accumulation_hours: null, temperature_c: null, ...processed },
  fuzzy: { state: 'UNCONFIGURED', score: null, label: null, reason: 'MODEL_NOT_CONFIGURED', model_state: 'UNCONFIGURED', trace: null },
  safety: { state: 'UNCONFIGURED', reason: 'SAFETY_NOT_CONFIGURED', model_state: 'UNCONFIGURED', trace: null },
});

describe('web OGI derivation (TEST_ONLY relative index)', () => {
  it('converts MQ-135 millivolts into a relative 0..100 index using the declared ADC reference', () => {
    const ogi = deriveOgi([gasPoint(1650)]);
    expect(ogi.state).toBe('READY');
    expect(ogi.value).toBe(50);
    expect(ogi.reason).toBe('READY');
  });

  it('keeps a real zero millivolt reading as zero instead of unavailable', () => {
    const ogi = deriveOgi([gasPoint(0)]);
    expect(ogi.state).toBe('READY');
    expect(ogi.value).toBe(0);
  });

  it('clamps readings above the ADC reference to the top of the index', () => {
    expect(deriveOgi([gasPoint(5000)]).value).toBe(100);
  });

  it('returns an explicit missing reason without history', () => {
    const ogi = deriveOgi([]);
    expect(ogi.state).toBe('UNAVAILABLE');
    expect(ogi.value).toBeNull();
    expect(ogi.reason).toBe('INPUT_MISSING');
  });

  it('returns an explicit reason for non-ready quality', () => {
    const ogi = deriveOgi([gasPoint(1650, { quality: 'UNCALIBRATED', reason: 'INVALID_PROFILE' })]);
    expect(ogi.state).toBe('UNAVAILABLE');
    expect(ogi.value).toBeNull();
    expect(ogi.reason).toBe('INPUT_NOT_READY');
  });

  it('rejects NaN and negative millivolts instead of coercing them to zero', () => {
    expect(deriveOgi([gasPoint(Number.NaN)]).reason).toBe('INPUT_INVALID');
    expect(deriveOgi([gasPoint(-5)]).reason).toBe('INPUT_INVALID');
    expect(deriveOgi([gasPoint(Number.NaN)]).value).toBeNull();
  });

  it('uses only the newest boot and never mixes earlier boots', () => {
    const ogi = deriveOgi([gasPoint(330, { boot_id: 'boot-old', sequence: '1' }), gasPoint(1650, { sequence: '2' })]);
    expect(ogi.value).toBe(50);
    expect(ogi.bootId).toBe('boot-a');
  });

  it('does not replace an invalid latest sample with an older valid sample', () => {
    const ogi = deriveOgi([gasPoint(1650, { sequence: '1' }), gasPoint(0, { sequence: '2', quality: 'STALE' })]);
    expect(ogi.value).toBeNull();
    expect(ogi.reason).toBe('INPUT_NOT_READY');
  });

  it('preserves an unknown origin instead of casting it to physical', () => {
    expect(deriveOgi([gasPoint(1650, { sample_origin: 'UNKNOWN' })]).origin).toBe('UNKNOWN');
  });

  it('reports no ppm or chemical concentration anywhere in the result', () => {
    const ogi = deriveOgi([gasPoint(1650)]);
    const serialized = JSON.stringify(ogi).toLowerCase();
    expect(serialized).not.toContain('ppm');
    expect(serialized).not.toContain('r0');
    expect(ogi.unit).toBe('indeks');
  });
});

describe('web accumulation derivation from real weight history', () => {
  it('measures monotonic hours since the last observed empty sample', () => {
    const accumulation = deriveAccumulationHours([weightPoint(0, 0), weightPoint(1, 3600), weightPoint(2, 7200), weightPoint(3, 10800)]);
    expect(accumulation.state).toBe('READY');
    expect(accumulation.value).toBeCloseTo(3, 6);
  });

  it('returns zero when the bin is currently empty', () => {
    const accumulation = deriveAccumulationHours([weightPoint(2, 0), weightPoint(0, 3600)]);
    expect(accumulation.state).toBe('READY');
    expect(accumulation.value).toBe(0);
  });

  it('returns unavailable instead of inventing a start when the window never observed empty', () => {
    const accumulation = deriveAccumulationHours([weightPoint(2, 0), weightPoint(3, 3600)]);
    expect(accumulation.state).toBe('UNAVAILABLE');
    expect(accumulation.value).toBeNull();
    expect(accumulation.reason).toBe('NO_EMPTY_OBSERVATION');
  });

  it('does not join accumulation across a boot change', () => {
    const accumulation = deriveAccumulationHours([
      weightPoint(0, 0, { boot_id: 'boot-old', sequence: '1' }),
      weightPoint(1, 3600, { boot_id: 'boot-old', sequence: '2' }),
      weightPoint(1.5, 0, { boot_id: 'boot-a', sequence: '1' }),
      weightPoint(2, 1800, { boot_id: 'boot-a', sequence: '2' }),
    ]);
    expect(accumulation.state).toBe('UNAVAILABLE');
    expect(accumulation.reason).toBe('NO_EMPTY_OBSERVATION');
    expect(accumulation.value).toBeNull();
  });

  it('refuses to compute when monotonic time is missing', () => {
    const accumulation = deriveAccumulationHours([weightPoint(0, 0), weightPoint(2, 3600, { monotonic_us: '' })]);
    expect(accumulation.reason).toBe('INPUT_TIME_UNTRUSTED');
    expect(accumulation.value).toBeNull();
  });

  it('refuses to compute when the monotonic clock runs backwards', () => {
    const accumulation = deriveAccumulationHours([
      weightPoint(0, 0, { monotonic_us: '5000' }),
      weightPoint(1, 1800, { monotonic_us: '4000' }),
      weightPoint(2, 3600, { monotonic_us: '6000' }),
    ]);
    expect(accumulation.reason).toBe('INPUT_TIME_UNTRUSTED');
  });

  it('resets accumulation after the bin is emptied again', () => {
    const accumulation = deriveAccumulationHours([weightPoint(0, 0), weightPoint(4, 7200), weightPoint(0, 9000), weightPoint(1, 9600)]);
    expect(accumulation.value).toBeCloseTo(600 / 3600, 6);
  });

  it('rejects a negative weight endpoint instead of treating it as empty', () => {
    const accumulation = deriveAccumulationHours([weightPoint(0, 0), weightPoint(-0.4, 3600)]);
    expect(accumulation.state).toBe('UNAVAILABLE');
    expect(accumulation.reason).toBe('INPUT_INVALID');
  });

  it('ignores negative middle samples and keeps the last valid empty observation', () => {
    const accumulation = deriveAccumulationHours([weightPoint(0, 0), weightPoint(-0.2, 1800), weightPoint(2, 3600)]);
    expect(accumulation.value).toBeCloseTo(1, 6);
  });

  it('uses the declared empty threshold instead of a hidden constant', () => {
    expect(deriveAccumulationHours([weightPoint(0, 0), weightPoint(TEST_ONLY_PROFILE.emptyWeightKg, 3600)]).value).toBe(0);
    expect(deriveAccumulationHours([weightPoint(0, 0), weightPoint(TEST_ONLY_PROFILE.emptyWeightKg + 0.01, 3600)]).value).toBeCloseTo(1, 6);
  });

  it('reports the observed window without claiming the full accumulation history', () => {
    const accumulation = deriveAccumulationHours([weightPoint(2, 0), weightPoint(3, 3600)]);
    expect(accumulation.observedFrom).toBe('2026-11-13T10:00:00.000Z');
    expect(accumulation.observedTo).toBe('2026-11-13T11:00:00.000Z');
    expect(accumulation.notes.join(' ')).toMatch(/jendela/i);
  });
});

describe('organic fuzzy evaluation (16-rule Mamdani TEST_ONLY model)', () => {
  const ready = (value: number, source = 'test') => ({ state: 'READY' as const, value, source });

  it('evaluates the declared model with real memberships, rules and a centroid', () => {
    const fis = evaluateOrganicFis({
      fill_pct: ready(100), weight_utilization_pct: ready(100), ogi: ready(100), accumulation_hours: ready(24),
    });
    expect(fis.state).toBe('READY');
    expect(fis.score).toBeGreaterThan(82);
    expect(fis.score).toBeLessThan(84);
    expect(fis.label).toBe('HIGH');
    expect(fis.memberships).toHaveLength(8);
    expect(fis.rules.length).toBeGreaterThan(0);
    expect(fis.aggregate?.area).toBeGreaterThan(0);
    expect(fis.aggregate?.centroid).toBeCloseTo(fis.score!, 6);
  });

  it('produces the LOW label for all-low inputs', () => {
    const fis = evaluateOrganicFis({
      fill_pct: ready(0), weight_utilization_pct: ready(0), ogi: ready(0), accumulation_hours: ready(0),
    });
    expect(fis.state).toBe('READY');
    expect(fis.label).toBe('LOW');
    expect(fis.score).toBeGreaterThan(16);
    expect(fis.score).toBeLessThan(18);
  });

  it('keeps every membership degree inside zero and one', () => {
    const fis = evaluateOrganicFis({
      fill_pct: ready(55), weight_utilization_pct: ready(40), ogi: ready(70), accumulation_hours: ready(12),
    });
    expect(fis.memberships.length).toBeGreaterThan(0);
    for (const membership of fis.memberships) {
      expect(membership.degree).toBeGreaterThanOrEqual(0);
      expect(membership.degree).toBeLessThanOrEqual(1);
    }
  });

  it('reports the missing input names instead of returning a zero score', () => {
    const fis = evaluateOrganicFis({
      fill_pct: { state: 'UNAVAILABLE', value: null, source: 'mcu' },
      weight_utilization_pct: ready(50), ogi: ready(40), accumulation_hours: ready(3),
    });
    expect(fis.state).toBe('UNAVAILABLE');
    expect(fis.score).toBeNull();
    expect(fis.label).toBeNull();
    expect(fis.missingInputs).toEqual(['fill_pct']);
  });

  it('rejects out-of-domain inputs the way the declared model does', () => {
    const fis = evaluateOrganicFis({
      fill_pct: ready(50), weight_utilization_pct: ready(140), ogi: ready(40), accumulation_hours: ready(3),
    });
    expect(fis.state).toBe('UNAVAILABLE');
    expect(fis.reason).toBe('INPUT_OUT_OF_DOMAIN');
    expect(fis.score).toBeNull();
  });

  it('increases the score as load inputs increase', () => {
    const low = evaluateOrganicFis({
      fill_pct: ready(10), weight_utilization_pct: ready(10), ogi: ready(10), accumulation_hours: ready(1),
    });
    const high = evaluateOrganicFis({
      fill_pct: ready(90), weight_utilization_pct: ready(90), ogi: ready(90), accumulation_hours: ready(20),
    });
    expect(high.score!).toBeGreaterThan(low.score!);
  });
});

describe('safety diagnostic (TEST_ONLY, non-actuating)', () => {
  it('flags attention at or above the declared alert threshold', () => {
    const safety = evaluateSafetyDiagnostic([tempPoint(60, 0)]);
    expect(safety.state).toBe('NEEDS_ATTENTION');
    expect(safety.reason).toBe('TEMPERATURE_ALERT');
    expect(safety.temperatureC).toBe(60);
  });

  it('flags the hysteresis band without clearing it', () => {
    const safety = evaluateSafetyDiagnostic([tempPoint(57, 0)]);
    expect(safety.state).toBe('NEEDS_ATTENTION');
    expect(safety.reason).toBe('TEMPERATURE_HYSTERESIS_BAND');
  });

  it('never claims a safe or clear state from a cool reading', () => {
    const safety = evaluateSafetyDiagnostic([tempPoint(30, 0), tempPoint(29, 60), tempPoint(28, 120)]);
    expect(safety.state).toBe('INSUFFICIENT_DATA');
    expect(safety.reason).toBe('NO_ALERT_OBSERVED');
    const serialized = JSON.stringify(safety);
    expect(serialized).not.toContain('Aman');
    expect(serialized).not.toMatch(/CLEAR/);
  });

  it('requires the declared number of consecutive clear cycles', () => {
    const safety = evaluateSafetyDiagnostic([tempPoint(30, 0)]);
    expect(safety.state).toBe('INSUFFICIENT_DATA');
    expect(safety.reason).toBe('CLEAR_CYCLES_NOT_MET');
    expect(safety.clearCyclesObserved).toBe(1);
    expect(safety.clearConsecutiveCycles).toBe(2);
  });

  it('breaks the clear cycle count on a gap', () => {
    const safety = evaluateSafetyDiagnostic([tempPoint(30, 0), tempPoint(20, 60, { quality: 'STALE' }), tempPoint(25, 120)]);
    expect(safety.clearCyclesObserved).toBe(1);
    expect(safety.reason).toBe('CLEAR_CYCLES_NOT_MET');
  });

  it('returns insufficient data for missing, invalid and non-physical temperature input', () => {
    expect(evaluateSafetyDiagnostic([]).reason).toBe('INPUT_MISSING');
    expect(evaluateSafetyDiagnostic([tempPoint(Number.NaN, 0)]).reason).toBe('INPUT_INVALID');
    expect(evaluateSafetyDiagnostic([tempPoint(30, 0, { sample_origin: 'SIMULATED' })]).reason).toBe('INPUT_ORIGIN_UNVERIFIED');
  });
});

describe('aggregated web diagnostics provenance', () => {
  it('labels the whole result as web-derived TEST_ONLY with an explicit profile version', () => {
    const diagnostics = buildTestOnlyDiagnostics({ results: [result({ fill_pct: 40, weight_utilization_pct: 30 })], monitoring: [monitoring()], history: { mq135: [gasPoint(1650)] } });
    expect(diagnostics.provenance).toBe('WEB_DERIVED_TEST_ONLY');
    expect(diagnostics.profile.id).toBe(TEST_ONLY_PROFILE.id);
    expect(diagnostics.profile.version).toBe(TEST_ONLY_PROFILE.version);
    expect(diagnostics.profile.deployment).toBe('TEST_ONLY');
    expect(diagnostics.fuzzy.deployment).toBe('TEST_ONLY');
    expect(diagnostics.safety.deployment).toBe('TEST_ONLY');
  });

  it('keeps each metric independently ready when another sensor is absent', () => {
    const diagnostics = buildTestOnlyDiagnostics({ monitoring: [monitoring({ sensor_id: 'hx711_organic', last_sequence: '3601', converted_value: { kg: 2 } })], history: { hx711_organic: [weightPoint(0, 0), weightPoint(2, 3600)] } });
    expect(diagnostics.accumulationHours.state).toBe('READY');
    expect(diagnostics.ogi.state).toBe('UNAVAILABLE');
    expect(diagnostics.ogi.reason).toBe('INPUT_MISSING');
  });

  it('passes the web OGI and accumulation into the fuzzy inputs with their own source labels', () => {
    const diagnostics = buildTestOnlyDiagnostics({
      results: [result({ fill_pct: 90, weight_utilization_pct: 85 })],
      monitoring: [monitoring({ last_sequence: '1' }), monitoring({ sensor_id: 'hx711_organic', last_sequence: '7201', converted_value: { kg: 4 } }), monitoring({ sensor_id: 'ultrasonic_organic', last_sequence: '1', converted_value: { distance_mm: 20 } })],
      history: { mq135: [gasPoint(1650)], hx711_organic: [weightPoint(0, 0), weightPoint(4, 7200)] },
    });
    expect(diagnostics.fuzzy.state).toBe('READY');
    expect(diagnostics.fuzzy.score).toBeGreaterThan(55);
    const ogiInput = diagnostics.fuzzy.inputs.find(input => input.input === 'ogi');
    const fillInput = diagnostics.fuzzy.inputs.find(input => input.input === 'fill_pct');
    expect(ogiInput?.source).toContain('WEB_DERIVED_TEST_ONLY');
    expect(fillInput?.source).toContain('WEB_DERIVED_TEST_ONLY');
  });

  it('marks B3 as insufficient input rather than clear', () => {
    const diagnostics = buildTestOnlyDiagnostics({ results: [result()], monitoring: [], history: {} });
    expect(diagnostics.b3.state).toBe('INSUFFICIENT_DATA');
    expect(diagnostics.b3.reason).toBe('INPUT_MISSING');
  });

  it('treats a stale monitoring sample as stale input for the gas index', () => {
    const diagnostics = buildTestOnlyDiagnostics({ results: [result()], monitoring: [monitoring({ fresh: false })], history: { mq135: [gasPoint(1650)] } });
    expect(diagnostics.ogi.state).toBe('UNAVAILABLE');
    expect(diagnostics.ogi.reason).toBe('INPUT_STALE');
  });
});