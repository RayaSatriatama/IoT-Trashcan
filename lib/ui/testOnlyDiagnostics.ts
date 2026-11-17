import type { FirmwareMonitoringSample, FirmwareResult, SensorHistoryPoint } from '../api/types';

/**
 * Pure web-side TEST_ONLY derivation of OGI, accumulation, the provisional
 * organic Mamdani model and the organic temperature safety diagnostic.
 *
 * Organic-model constants below are copied from declared repository sources:
 * - `firmware/lib/models/src/models/prototype_defaults.cpp` (membership sets,
 *   16-rule table, output sets, labels, centroid grid, safety channel).
 * - `firmware/lib/processing/src/processing/processing_engine.h` (OGI relative
 *   index scale, ADC reference, 0.05 kg empty threshold).
 * - `docs/prototype-operation.md` (prototype assumptions, TEST_ONLY deployment).
 *
 * The separate MQ135 helper uses an explicitly assumed reference curve.
 * Nothing here is a production model or physical gas calibration.
 * Values that cannot be derived from real inputs stay unavailable with a
 * specific reason instead of becoming zero.
 */

export type MetricState = 'READY' | 'UNAVAILABLE';

// Reference defaults: GeorgK/MQ135 MQ135.h; circuit equation: Winsen MQ135 manual.
// shortcut: R0 is an assumed reference, replace it after real gas calibration.
export const MQ135_PPM_PROFILE = Object.freeze({
  deployment: 'TEST_ONLY', target: 'CO2_EQUIVALENT',
  supplyV: 5, loadKohm: 10, r0Kohm: 76.63,
  coefficientA: 116.6020682, exponentB: -2.769034857,
});
export const MQ135_PPM_NOTE = `Vc=${MQ135_PPM_PROFILE.supplyV} V; RL=${MQ135_PPM_PROFILE.loadKohm} kΩ; R0=${MQ135_PPM_PROFILE.r0Kohm} kΩ; A=${MQ135_PPM_PROFILE.coefficientA}; B=${MQ135_PPM_PROFILE.exponentB}. R0 adalah asumsi referensi, bukan kalibrasi sensor ini. Estimasi tidak dikompensasi suhu/RH dan tidak untuk keputusan safety.`;

/** @deprecated Research-only historical formula. Never use for monitoring, history or classification. */
export function estimateMq135Ppm(adcMv: unknown): number | null {
  const p = MQ135_PPM_PROFILE;
  if (typeof adcMv !== 'number' || !Number.isFinite(adcMv) ||
      adcMv <= 0 || adcMv >= p.supplyV * 1000) return null;
  const resistanceKohm = (p.supplyV / (adcMv / 1000) - 1) * p.loadKohm;
  const ppm = p.coefficientA * Math.pow(resistanceKohm / p.r0Kohm, p.exponentB);
  return Number.isFinite(ppm) && ppm >= 0 ? ppm : null;
}

export type DiagnosticReason =
  | 'READY'
  | 'INPUT_MISSING'
  | 'INPUT_NOT_READY'
  | 'INPUT_INVALID'
  | 'INPUT_STALE'
  | 'INPUT_TIME_UNTRUSTED'
  | 'INPUT_ORIGIN_UNVERIFIED'
  | 'INPUT_OUT_OF_DOMAIN'
  | 'NO_EMPTY_OBSERVATION'
  | 'CLEAR_CYCLES_NOT_MET'
  | 'NO_ALERT_OBSERVED'
  | 'TEMPERATURE_ALERT'
  | 'TEMPERATURE_HYSTERESIS_BAND';

export type SensorOrigin = 'PHYSICAL' | 'SIMULATED' | 'UNKNOWN';

export interface MetricInput {
  state: MetricState;
  value: number | null;
  source: string;
}

export interface DerivedMetric {
  state: MetricState;
  value: number | null;
  unit: string;
  reason: DiagnosticReason;
  source: string;
  origin: SensorOrigin | null;
  bootId: string | null;
  observedFrom: string | null;
  observedTo: string | null;
  notes: string[];
}

export interface FisMembership {
  input: string;
  set: string;
  degree: number;
}

export interface FisRuleHit {
  id: number;
  strength: number;
  consequent: 'LOW' | 'MID' | 'HIGH';
}

export interface FisEvaluation {
  state: MetricState;
  reason: DiagnosticReason;
  score: number | null;
  label: 'LOW' | 'MID' | 'HIGH' | null;
  memberships: FisMembership[];
  rules: FisRuleHit[];
  aggregate: { area: number; centroid: number } | null;
  missingInputs: string[];
  inputs: (MetricInput & { input: string })[];
  deployment: 'TEST_ONLY';
}

export interface SafetyDiagnostic {
  state: 'NEEDS_ATTENTION' | 'INSUFFICIENT_DATA';
  reason: DiagnosticReason;
  temperatureC: number | null;
  alertAtOrAboveC: number;
  clearBelowC: number;
  clearConsecutiveCycles: number;
  clearCyclesObserved: number;
  origin: SensorOrigin | null;
  deployment: 'TEST_ONLY';
  notes: string[];
}

export interface TestOnlyDiagnostics {
  provenance: 'WEB_DERIVED_TEST_ONLY';
  profile: { id: string; version: number; deployment: 'TEST_ONLY' };
  ogi: DerivedMetric;
  fillPercent: DerivedMetric;
  weightUtilization: DerivedMetric;
  accumulationHours: DerivedMetric;
  fuzzy: FisEvaluation;
  safety: SafetyDiagnostic;
  b3: { state: 'INSUFFICIENT_DATA'; reason: DiagnosticReason; notes: string[] };
}

type Shape = 'triangle' | 'trapezoid';
interface MembershipSpec { id: string; shape: Shape; points: readonly number[] }
interface InputSpec { id: string; label: string; unit: string; domain: readonly [number, number]; sets: readonly MembershipSpec[] }
interface RuleSpec { id: number; sets: readonly [number, number, number, number]; consequent: 0 | 1 | 2 }

const percentSets: readonly MembershipSpec[] = [
  { id: 'LOW', shape: 'trapezoid', points: [0, 0, 30, 70] },
  { id: 'HIGH', shape: 'trapezoid', points: [30, 70, 100, 100] },
];
const hourSets: readonly MembershipSpec[] = [
  { id: 'LOW_H', shape: 'trapezoid', points: [0, 0, 6, 18] },
  { id: 'HIGH_H', shape: 'trapezoid', points: [6, 18, 24, 24] },
];

const fisInputs: readonly InputSpec[] = [
  { id: 'fill_pct', label: 'Keterisian', unit: '%', domain: [0, 100], sets: percentSets },
  { id: 'weight_utilization_pct', label: 'Utilisasi berat', unit: '%', domain: [0, 100], sets: percentSets },
  { id: 'ogi', label: 'OGI', unit: 'indeks', domain: [0, 100], sets: percentSets },
  { id: 'accumulation_hours', label: 'Akumulasi', unit: 'jam', domain: [0, 24], sets: hourSets },
];

const LOW = 0, HIGH = 1;
const fisRules: readonly RuleSpec[] = [
  { id: 1, sets: [LOW, LOW, LOW, LOW], consequent: 0 },
  { id: 2, sets: [LOW, LOW, LOW, HIGH], consequent: 1 },
  { id: 3, sets: [LOW, LOW, HIGH, LOW], consequent: 1 },
  { id: 4, sets: [LOW, LOW, HIGH, HIGH], consequent: 2 },
  { id: 5, sets: [LOW, HIGH, LOW, LOW], consequent: 2 },
  { id: 6, sets: [LOW, HIGH, LOW, HIGH], consequent: 2 },
  { id: 7, sets: [LOW, HIGH, HIGH, LOW], consequent: 2 },
  { id: 8, sets: [LOW, HIGH, HIGH, HIGH], consequent: 2 },
  { id: 9, sets: [HIGH, LOW, LOW, LOW], consequent: 2 },
  { id: 10, sets: [HIGH, LOW, LOW, HIGH], consequent: 2 },
  { id: 11, sets: [HIGH, LOW, HIGH, LOW], consequent: 2 },
  { id: 12, sets: [HIGH, LOW, HIGH, HIGH], consequent: 2 },
  { id: 13, sets: [HIGH, HIGH, LOW, LOW], consequent: 2 },
  { id: 14, sets: [HIGH, HIGH, LOW, HIGH], consequent: 2 },
  { id: 15, sets: [HIGH, HIGH, HIGH, LOW], consequent: 2 },
  { id: 16, sets: [HIGH, HIGH, HIGH, HIGH], consequent: 2 },
];

const outputSets: readonly MembershipSpec[] = [
  { id: 'LOW', shape: 'trapezoid', points: [0, 0, 20, 45] },
  { id: 'MID', shape: 'triangle', points: [20, 50, 80] },
  { id: 'HIGH', shape: 'trapezoid', points: [55, 80, 100, 100] },
];

export const TEST_ONLY_PROFILE = {
  id: 'WEB_TEST_ONLY_ORGANIC_PROTOTYPE',
  version: 2,
  deployment: 'TEST_ONLY' as const,
  adcReferenceMv: 3300,
  ogiScale: 100,
  emptyWeightKg: 0.05,
  // Explicit web assumptions, not a readback of the device's saved calibration.
  bin: { capacityKg: 5, emptyDistanceMm: 230.153, fullDistanceMm: 20 },
  maxCaptureAgeMs: 30_000,
  centroidGridPoints: 101,
  areaEpsilon: 1e-4,
  output: { id: 'FIS_SCORE', domain: [0, 100] as const, sets: outputSets, labels: [{ id: 'LOW' as const, minimumInclusive: 0 }, { id: 'MID' as const, minimumInclusive: 20 }, { id: 'HIGH' as const, minimumInclusive: 55 }] },
  safety: { signal: 'temperature_c', alertAtOrAboveC: 60, clearBelowC: 55, clearConsecutiveCycles: 2 },
  sourceAnchors: [
    'firmware/lib/models/src/models/prototype_defaults.cpp',
    'firmware/lib/processing/src/processing/processing_engine.h',
    'docs/prototype-operation.md',
    'firmware/lib/acquisition/src/acquisition/default_profiles.h',
  ],
} as const;

export type TestOnlyProfile = typeof TEST_ONLY_PROFILE;

const numeric = (value: unknown): number | null => (typeof value === 'number' && Number.isFinite(value) ? value : null);
const convertedValue = (point: SensorHistoryPoint, key: string): number | null => numeric(point.converted?.[key]);
const monotonic = (value: string): bigint | null => (/^(0|[1-9][0-9]*)$/.test(value) ? BigInt(value) : null);
const trustedIso = (point: SensorHistoryPoint): string | null => {
  if (point.time_quality !== 'SYNCED' && point.time_quality !== 'RECONSTRUCTED') return null;
  if (!point.recorded_at) return null;
  const parsed = Date.parse(point.recorded_at);
  return Number.isFinite(parsed) ? new Date(parsed).toISOString() : null;
};

/** The API returns history in observation order; only the trailing run that
 * shares the newest boot identity is used, so nothing is joined across boots. */
function trailingBootRun(points: readonly SensorHistoryPoint[]): { bootId: string | null; run: readonly SensorHistoryPoint[] } {
  if (points.length === 0) return { bootId: null, run: [] };
  const bootId = points[points.length - 1].boot_id;
  let start = points.length - 1;
  while (start > 0 && points[start - 1].boot_id === bootId) start -= 1;
  return { bootId, run: points.slice(start) };
}

function unavailable(reason: DiagnosticReason, source: string, bootId: string | null, notes: string[] = []): DerivedMetric {
  return { state: 'UNAVAILABLE', value: null, unit: '', reason, source, origin: null, bootId, observedFrom: null, observedTo: null, notes };
}

/**
 * Relative organic gas index 0..100 from MQ-135 ADC millivolts.
 * This is the prototype relative-voltage assumption declared in
 * `docs/prototype-operation.md`, not ppm, not R0, and not a chemical hazard.
 */
export function deriveOgi(points: readonly SensorHistoryPoint[], profile: TestOnlyProfile = TEST_ONLY_PROFILE): DerivedMetric {
  const source = 'WEB_DERIVED_TEST_ONLY.mq135.converted.adc_mv';
  const { bootId, run } = trailingBootRun(points);
  if (run.length === 0) return unavailable('INPUT_MISSING', source, null, ['Riwayat MQ-135 belum tersedia.']);

  const latest = run[run.length - 1];
  const candidate = latest;
  const millivolts = convertedValue(candidate, 'adc_mv');
  if (candidate.quality === 'READY' && millivolts !== null && millivolts >= 0) {
    const indexValue = Math.min(profile.ogiScale, Math.max(0, (millivolts / profile.adcReferenceMv) * profile.ogiScale));
    return {
      state: 'READY', value: indexValue, unit: 'indeks', reason: 'READY', source,
      origin: candidate.sample_origin, bootId,
      observedFrom: trustedIso(candidate), observedTo: trustedIso(candidate),
      notes: ['Indeks relatif tegangan ADC; bukan konsentrasi gas dan bukan kadar zat tertentu.'],
    };
  }

  const latestMillivolts = convertedValue(latest, 'adc_mv');
  const latestRaw = latest.converted?.adc_mv;
  const reason: DiagnosticReason = latest.quality !== 'READY'
    ? 'INPUT_NOT_READY'
    : latestMillivolts === null
      ? (typeof latestRaw === 'number' ? 'INPUT_INVALID' : 'INPUT_MISSING')
      : 'INPUT_INVALID';
  return unavailable(reason, source, bootId, ['Sampel MQ-135 terbaru tidak dapat dipakai.']);
}

/**
 * Hours since the last observed empty weight inside one boot. The empty
 * threshold is the declared prototype constant; ultrasonic fill geometry is
 * not published by the API, so emptiness is judged from weight only.
 */
export function deriveAccumulationHours(points: readonly SensorHistoryPoint[], profile: TestOnlyProfile = TEST_ONLY_PROFILE): DerivedMetric {
  const source = 'WEB_DERIVED_TEST_ONLY.hx711_organic.converted.kg';
  const { bootId, run } = trailingBootRun(points);
  if (run.length === 0) return unavailable('INPUT_MISSING', source, null, ['Riwayat berat belum tersedia.']);

  const latest = run[run.length - 1];
  const latestWeight = convertedValue(latest, 'kg');
  if (latest.quality !== 'READY') return unavailable('INPUT_NOT_READY', source, bootId, ['Sampel berat terbaru belum siap.']);
  if (latestWeight === null) return unavailable('INPUT_MISSING', source, bootId, ['Nilai berat terbaru tidak tersedia.']);
  if (latestWeight < 0) return unavailable('INPUT_INVALID', source, bootId, ['Berat negatif; kemungkinan tare atau sensor bermasalah.']);

  const usable: { point: SensorHistoryPoint; weight: number; at: bigint }[] = [];
  let negativeIgnored = 0;
  for (const candidate of run) {
    if (candidate.quality !== 'READY') continue;
    const weight = convertedValue(candidate, 'kg');
    if (weight === null) continue;
    if (weight < 0) { negativeIgnored += 1; continue; }
    const at = monotonic(candidate.monotonic_us);
    if (at === null) return unavailable('INPUT_TIME_UNTRUSTED', source, bootId, ['Waktu monoton tidak tersedia untuk sebagian sampel.']);
    usable.push({ point: candidate, weight, at });
  }
  if (usable.length === 0) return unavailable('INPUT_INVALID', source, bootId, ['Tidak ada sampel berat yang valid.']);

  const endpoint = usable[usable.length - 1];
  for (let index = 1; index < usable.length; index += 1) {
    if (usable[index].at < usable[index - 1].at) return unavailable('INPUT_TIME_UNTRUSTED', source, bootId, ['Jam monoton mundur; urutan sampel tidak tepercaya.']);
  }

  const notes = ['Deteksi kosong memakai ambang berat prototipe; geometri ultrasonik tidak tersedia di web.'];
  if (negativeIgnored > 0) notes.push(`${negativeIgnored} sampel berat negatif diabaikan, bukan dianggap kosong.`);
  const window: Pick<DerivedMetric, 'observedFrom' | 'observedTo'> = { observedFrom: trustedIso(usable[0].point), observedTo: trustedIso(endpoint.point) };

  if (endpoint.weight <= profile.emptyWeightKg) {
    return { state: 'READY', value: 0, unit: 'jam', reason: 'READY', source, origin: endpoint.point.sample_origin, bootId, ...window, notes: [...notes, 'Wadah teramati kosong pada sampel terbaru.'] };
  }

  let lastEmpty: typeof usable[number] | null = null;
  for (const sample of usable) if (sample.weight <= profile.emptyWeightKg) lastEmpty = sample;
  if (lastEmpty === null) {
    const spanHours = Number(endpoint.at - usable[0].at) / 3_600_000_000;
    return {
      state: 'UNAVAILABLE', value: null, unit: 'jam', reason: 'NO_EMPTY_OBSERVATION', source,
      origin: endpoint.point.sample_origin, bootId, ...window,
      notes: [...notes, `Jendela teramati ${spanHours.toFixed(2)} jam tanpa sampel kosong; awal akumulasi tidak diketahui dan tidak diperkirakan.`],
    };
  }

  const hours = Number(endpoint.at - lastEmpty.at) / 3_600_000_000;
  return {
    state: 'READY', value: hours, unit: 'jam', reason: 'READY', source,
    origin: endpoint.point.sample_origin, bootId, ...window,
    notes: [...notes, 'Diukur dari sampel kosong terakhir dalam boot yang sama; bukan total riwayat perangkat.'],
  };
}

function membership(spec: MembershipSpec, value: number): number {
  if (!Number.isFinite(value)) return 0;
  const points = spec.points;
  if (spec.shape === 'triangle') {
    if (value <= points[0] || value >= points[2]) return 0;
    if (value === points[1]) return 1;
    if (value < points[1]) return (value - points[0]) / (points[1] - points[0]);
    return (points[2] - value) / (points[2] - points[1]);
  }
  if (value < points[0] || value > points[3]) return 0;
  if (points[0] === points[1] && value <= points[1]) return 1;
  if (value < points[1]) return (value - points[0]) / (points[1] - points[0]);
  if (value <= points[2]) return 1;
  if (points[2] === points[3] && value >= points[2]) return 1;
  return (points[3] - value) / (points[3] - points[2]);
}

/** Mamdani evaluation mirroring `fuzzy::MamdaniEvaluator::evaluate`. */
export function evaluateOrganicFis(
  inputs: Record<string, MetricInput>,
  profile: TestOnlyProfile = TEST_ONLY_PROFILE,
): FisEvaluation {
  const ordered = fisInputs.map(spec => ({ spec, input: inputs[spec.id] ?? { state: 'UNAVAILABLE' as const, value: null, source: 'UNAVAILABLE' } }));
  const base: FisEvaluation = {
    state: 'UNAVAILABLE', reason: 'INPUT_MISSING', score: null, label: null,
    memberships: [], rules: [], aggregate: null, missingInputs: [],
    inputs: ordered.map(item => ({ input: item.spec.id, ...item.input })), deployment: 'TEST_ONLY',
  };

  for (const { spec, input } of ordered) {
    if (input.state !== 'READY' || numeric(input.value) === null) base.missingInputs.push(spec.id);
  }
  if (base.missingInputs.length > 0) return base;
  for (const { spec, input } of ordered) {
    const value = input.value as number;
    if (value < spec.domain[0] || value > spec.domain[1]) return { ...base, reason: 'INPUT_OUT_OF_DOMAIN', missingInputs: [spec.id] };
  }

  const memberships: FisMembership[] = [];
  const degrees: number[][] = ordered.map(({ spec, input }) => spec.sets.map(set => {
    const degree = membership(set, input.value as number);
    memberships.push({ input: spec.id, set: set.id, degree });
    return degree;
  }));

  const strengths = [0, 0, 0];
  const rules: FisRuleHit[] = [];
  for (const rule of fisRules) {
    let strength = 1;
    for (let index = 0; index < 4; index += 1) strength = Math.min(strength, degrees[index][rule.sets[index]]);
    if (strength <= 0) continue;
    strengths[rule.consequent] = Math.max(strengths[rule.consequent], strength);
    rules.push({ id: rule.id, strength, consequent: (['LOW', 'MID', 'HIGH'] as const)[rule.consequent] });
  }

  const [minimum, maximum] = profile.output.domain;
  const grid = profile.centroidGridPoints;
  const step = (maximum - minimum) / (grid - 1);
  let area = 0;
  let moment = 0;
  let previous = 0;
  let previousX: number = minimum;
  for (let index = 0; index < grid; index += 1) {
    const x = minimum + step * index;
    let aggregated = 0;
    for (let setIndex = 0; setIndex < profile.output.sets.length; setIndex += 1) {
      aggregated = Math.max(aggregated, Math.min(membership(profile.output.sets[setIndex], x), strengths[setIndex]));
    }
    if (index !== 0) {
      const width = x - previousX;
      area += width * (previous + aggregated) * 0.5;
      moment += width * (previousX * previous + x * aggregated) * 0.5;
    }
    previous = aggregated;
    previousX = x;
  }

  if (!Number.isFinite(area) || area <= profile.areaEpsilon) {
    return { ...base, memberships, rules, reason: 'INPUT_INVALID' };
  }
  const centroid = moment / area;
  if (!Number.isFinite(centroid)) return { ...base, memberships, rules, reason: 'INPUT_INVALID' };
  const label = [...profile.output.labels].reverse().find(item => centroid >= item.minimumInclusive)?.id ?? 'LOW';
  return {
    state: 'READY', reason: 'READY', score: centroid, label,
    memberships, rules, aggregate: { area, centroid }, missingInputs: [],
    inputs: ordered.map(item => ({ input: item.spec.id, ...item.input })), deployment: 'TEST_ONLY',
  };
}

/**
 * Organic temperature diagnostic. It can only say "needs attention" or
 * "insufficient data"; a TEST_ONLY clear is never reported as safe and never
 * drives an actuator.
 */
export function evaluateSafetyDiagnostic(points: readonly SensorHistoryPoint[], profile: TestOnlyProfile = TEST_ONLY_PROFILE): SafetyDiagnostic {
  const settings = profile.safety;
  const base: SafetyDiagnostic = {
    state: 'INSUFFICIENT_DATA', reason: 'INPUT_MISSING', temperatureC: null,
    alertAtOrAboveC: settings.alertAtOrAboveC, clearBelowC: settings.clearBelowC,
    clearConsecutiveCycles: settings.clearConsecutiveCycles, clearCyclesObserved: 0,
    origin: null, deployment: 'TEST_ONLY',
    notes: ['Diagnostik TEST_ONLY tanpa aktuasi; status aman produksi tidak dinyatakan.'],
  };
  const { run } = trailingBootRun(points);
  if (run.length === 0) return { ...base, notes: [...base.notes, 'Riwayat suhu organik belum tersedia.'] };

  const latest = run[run.length - 1];
  const raw = latest.converted?.temperature_c;
  if (typeof raw === 'number' && !Number.isFinite(raw)) return { ...base, reason: 'INPUT_INVALID', origin: latest.sample_origin };
  const temperature = numeric(raw);
  if (temperature === null) return { ...base, reason: 'INPUT_MISSING', origin: latest.sample_origin };
  if (latest.quality !== 'READY') return { ...base, reason: 'INPUT_NOT_READY', temperatureC: temperature, origin: latest.sample_origin };
  if (latest.sample_origin !== 'PHYSICAL') return { ...base, reason: 'INPUT_ORIGIN_UNVERIFIED', temperatureC: temperature, origin: latest.sample_origin };

  const withTemperature = { ...base, temperatureC: temperature, origin: latest.sample_origin };
  if (temperature >= settings.alertAtOrAboveC) return { ...withTemperature, state: 'NEEDS_ATTENTION', reason: 'TEMPERATURE_ALERT' };
  if (temperature >= settings.clearBelowC) return { ...withTemperature, state: 'NEEDS_ATTENTION', reason: 'TEMPERATURE_HYSTERESIS_BAND' };

  let clearCycles = 0;
  for (let index = run.length - 1; index >= 0; index -= 1) {
    const candidate = run[index];
    const value = convertedValue(candidate, 'temperature_c');
    if (candidate.quality !== 'READY' || candidate.sample_origin !== 'PHYSICAL' || value === null || value >= settings.clearBelowC) break;
    clearCycles += 1;
  }
  if (clearCycles >= settings.clearConsecutiveCycles) return { ...withTemperature, reason: 'NO_ALERT_OBSERVED', clearCyclesObserved: clearCycles };
  return { ...withTemperature, reason: 'CLEAR_CYCLES_NOT_MET', clearCyclesObserved: clearCycles };
}

export function buildTestOnlyDiagnostics(input: {
  results?: readonly FirmwareResult[]; // Historical MCU output is deliberately not a web input.
  monitoring?: readonly FirmwareMonitoringSample[];
  history?: Readonly<Record<string, readonly SensorHistoryPoint[]>>;
}): TestOnlyDiagnostics {
  const history = input.history ?? {};
  const monitoring = input.monitoring ?? [];
  const current = (id: string): { sample?: FirmwareMonitoringSample; reason?: DiagnosticReason } => {
    const candidates = monitoring.filter(sample => sample.sensor_id === id);
    if (candidates.length !== 1) return { reason: candidates.length ? 'INPUT_INVALID' : 'INPUT_MISSING' };
    const sample = candidates[0];
    if (!sample.boot_id || monotonic(sample.last_sequence) === null) return { reason: 'INPUT_INVALID' };
    if (sample.quality !== 'READY') return { sample, reason: 'INPUT_NOT_READY' };
    if (sample.future_clock_skew) return { sample, reason: 'INPUT_TIME_UNTRUSTED' };
    if (sample.fresh === false || numeric(sample.age_ms) === null || sample.age_ms! < 0 || sample.age_ms! > TEST_ONLY_PROFILE.maxCaptureAgeMs) {
      return { sample, reason: 'INPUT_STALE' };
    }
    return { sample };
  };
  const ownedHistory = (id: string): { points: readonly SensorHistoryPoint[]; reason?: DiagnosticReason } => {
    const view = current(id);
    if (view.reason || !view.sample) return { points: [], reason: view.reason ?? 'INPUT_MISSING' };
    const sample = view.sample;
    const points = (history[id] ?? []).filter(point => point.boot_id === sample.boot_id);
    if (!points.length) return { points: [], reason: 'INPUT_MISSING' };
    const seen = new Set<string>();
    for (const point of points) {
      const sequence = monotonic(point.sequence);
      if (sequence === null || sequence > BigInt(sample.last_sequence) || seen.has(point.sequence) || point.sample_origin !== sample.sample_origin) {
        return { points: [], reason: 'INPUT_INVALID' };
      }
      seen.add(point.sequence);
    }
    const ordered = [...points].sort((a, b) => BigInt(a.sequence) < BigInt(b.sequence) ? -1 : BigInt(a.sequence) > BigInt(b.sequence) ? 1 : 0);
    const latest = ordered[ordered.length - 1];
    if (latest.sequence !== sample.last_sequence) return { points: [], reason: 'INPUT_MISSING' };
    if (!trustedIso(latest)) return { points: [], reason: 'INPUT_TIME_UNTRUSTED' };
    return { points: ordered };
  };
  const historyMetric = (id: string, derive: (points: readonly SensorHistoryPoint[]) => DerivedMetric): DerivedMetric => {
    const owned = ownedHistory(id);
    return owned.reason ? unavailable(owned.reason, `WEB_DERIVED_TEST_ONLY.${id}`, current(id).sample?.boot_id ?? null) : derive(owned.points);
  };
  const fromSnapshot = (id: string, key: string, unit: string, convert: (value: number) => number): DerivedMetric => {
    const view = current(id);
    const source = `WEB_DERIVED_TEST_ONLY.${id}.${key}`;
    if (view.reason || !view.sample) return unavailable(view.reason ?? 'INPUT_MISSING', source, view.sample?.boot_id ?? null);
    const value = numeric(view.sample.converted_value?.[key]);
    if (value === null) return unavailable('INPUT_INVALID', source, view.sample.boot_id);
    const converted = convert(value);
    if (!Number.isFinite(converted)) return unavailable('INPUT_INVALID', source, view.sample.boot_id);
    return { state: 'READY', value: converted, unit, reason: 'READY', source, origin: view.sample.sample_origin,
      bootId: view.sample.boot_id, observedFrom: null, observedTo: null,
      notes: ['Asumsi profil web TEST_ONLY, bukan pembacaan profil kalibrasi tersimpan perangkat.'] };
  };
  const { capacityKg, emptyDistanceMm, fullDistanceMm } = TEST_ONLY_PROFILE.bin;
  const hasPublishedFill = Object.prototype.hasOwnProperty.call(current('ultrasonic_organic').sample?.converted_value ?? {}, 'fill_percent');
  const fillPercent = hasPublishedFill ? fromSnapshot('ultrasonic_organic', 'fill_percent', '%', value => value <= 100 ? value : NaN) : fromSnapshot('ultrasonic_organic', 'distance_mm', '%', distance => {
    if (!(emptyDistanceMm > fullDistanceMm)) return NaN;
    return Math.max(0, Math.min(100, 100 * (emptyDistanceMm - distance) / (emptyDistanceMm - fullDistanceMm)));
  });
  // Absolute weight: the load cell reports a signed offset around tare.
  const weightUtilization = fromSnapshot('hx711_organic', 'kg', '%', kg => capacityKg > 0 ? 100 * Math.abs(kg) / capacityKg : NaN);
  if (hasPublishedFill && fillPercent.state === 'READY') fillPercent.notes = ['Keterisian engineering yang diterbitkan firmware; dipakai sebagai input analisis web TEST_ONLY. Tidak direkonstruksi menjadi jarak.'];
  const ogi = historyMetric('mq135', deriveOgi);
  const accumulationHours = historyMetric('hx711_organic', deriveAccumulationHours);
  const fisInput = (metric: DerivedMetric): MetricInput => ({ state: metric.state, value: metric.value, source: metric.source });
  const fuzzy = evaluateOrganicFis({ fill_pct: fisInput(fillPercent), weight_utilization_pct: fisInput(weightUtilization),
    ogi: fisInput(ogi), accumulation_hours: fisInput(accumulationHours) });
  const temperature = ownedHistory('sht31_44');
  const safety = evaluateSafetyDiagnostic(temperature.points);
  if (temperature.reason) safety.reason = temperature.reason;
  return {
    provenance: 'WEB_DERIVED_TEST_ONLY',
    profile: { id: TEST_ONLY_PROFILE.id, version: TEST_ONLY_PROFILE.version, deployment: TEST_ONLY_PROFILE.deployment },
    ogi, fillPercent, weightUtilization, accumulationHours, fuzzy, safety,
    b3: { state: 'INSUFFICIENT_DATA', reason: 'INPUT_MISSING',
      notes: ['Kanal B3 dan anorganik tidak tersedia pada perangkat ini; tidak ada status aman yang dinyatakan.'] },
  };
}
