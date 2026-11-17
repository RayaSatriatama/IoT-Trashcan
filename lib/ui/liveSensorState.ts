import type { FirmwareMonitoringSample, SensorState } from '../api/types';
import { SENSOR_METRICS, sensorMetricValue } from './sensorHistory';
import { formatFirmwareMetric, firmwareReason } from './firmwarePresentation';

// UI-only display policy. The API owns freshness; this window never overrides it.
export const RECENT_SAMPLE_MS = 30_000;

export interface LiveSampleView {
  sensorId: FirmwareMonitoringSample['sensor_id'];
  identity: string;
  state: SensorState;
  freshnessLabel: string;
  ageLabel: string;
  recencyLabel: string | null;
  originLabel: string;
  orderingNote: string | null;
  reasonLabel: string | null;
  values: { label: string; text: string }[];
}

/** Internal traceability identity. Never rendered as a human label. */
export function captureIdentity(sample: FirmwareMonitoringSample): string {
  return `${sample.boot_id}\0${sample.last_sequence}`;
}

export function formatSampleAge(ageMs: number | null | undefined): string {
  if (typeof ageMs !== 'number' || !Number.isFinite(ageMs) || ageMs < 0) return 'tidak diketahui';
  if (ageMs < 1_000) return '<1 detik';
  if (ageMs < 60_000) return `${Math.floor(ageMs / 1_000)} detik`;
  if (ageMs < 3_600_000) return `${Math.floor(ageMs / 60_000)} menit`;
  if (ageMs < 86_400_000) return `${Math.floor(ageMs / 3_600_000)} jam`;
  return `${Math.floor(ageMs / 86_400_000)} hari`;
}

function sampleState(quality: string): SensorState {
  if (quality === 'DISABLED') return 'DISABLED';
  if (quality === 'STALE') return 'STALE';
  return quality === 'READY' ? 'VALID' : 'INVALID';
}

function freshnessLabel(fresh: boolean | null): string {
  if (fresh === true) return 'Segar menurut API';
  if (fresh === false) return 'Tidak segar menurut API';
  return 'Kesegaran belum ditetapkan API';
}

function ageLabel(sample: FirmwareMonitoringSample): string {
  if (sample.future_clock_skew) return 'Umur contoh tidak dapat dipercaya: jam contoh di masa depan';
  const age = formatSampleAge(sample.age_ms);
  return age === 'tidak diketahui' ? 'Umur contoh tidak diketahui' : `Umur sampel (estimasi): ${age}`;
}

function recencyLabel(sample: FirmwareMonitoringSample): string | null {
  if (sample.future_clock_skew) return null;
  if (typeof sample.age_ms !== 'number' || !Number.isFinite(sample.age_ms) || sample.age_ms < 0) return null;
  return sample.age_ms <= RECENT_SAMPLE_MS ? 'Terbaru (≤30 detik)' : 'Tertunda (>30 detik)';
}

const originLabels: Record<FirmwareMonitoringSample['sample_origin'], string> = {
  PHYSICAL: 'Fisik (metadata firmware)',
  SIMULATED: 'Simulasi firmware',
  UNKNOWN: 'Asal tidak diketahui',
};

export function describeLiveSample(sample: FirmwareMonitoringSample, elapsedMs = 0): LiveSampleView {
  sample = { ...sample, age_ms: effectiveSampleAge(sample.age_ms, elapsedMs) };
  const metrics = SENSOR_METRICS[sample.sensor_id] ?? [];
  const converted = sample.quality === 'READY' ? sample.converted_value : null;
  const values = metrics.map(metric => {
    const value = sensorMetricValue(converted, metric);
    return { label: metric.label, text: formatFirmwareMetric(typeof value === 'number' && Number.isFinite(value) ? value : null, metric.unit) };
  });
  return {
    sensorId: sample.sensor_id,
    identity: captureIdentity(sample),
    state: sampleState(sample.quality),
    freshnessLabel: freshnessLabel(sample.fresh),
    ageLabel: ageLabel(sample),
    recencyLabel: recencyLabel(sample),
    originLabel: originLabels[sample.sample_origin] ?? 'Asal tidak diketahui',
    orderingNote: sample.ordering_unknown ? 'Urutan lintas boot tidak diketahui' : null,
    reasonLabel: sample.quality === 'READY' ? null : firmwareReason(sample.reason ?? ''),
    values,
  };
}

/** A new capture is animatable only when it is a genuinely recent READY reading. */
function isArrival(previous: FirmwareMonitoringSample | undefined, current: FirmwareMonitoringSample): boolean {
  if (!previous || captureIdentity(previous) === captureIdentity(current)) return false;
  if (!/^(0|[1-9][0-9]*)$/.test(current.last_sequence)) return false;
  if (current.boot_id === previous.boot_id && /^(0|[1-9][0-9]*)$/.test(previous.last_sequence) &&
      BigInt(current.last_sequence) <= BigInt(previous.last_sequence)) return false;
  if (current.quality !== 'READY' || current.future_clock_skew || current.fresh === false) return false;
  const age = current.age_ms;
  return typeof age === 'number' && Number.isFinite(age) && age >= 0 && age <= RECENT_SAMPLE_MS;
}

export function arrivedSensorIds(
  previous: ReadonlyMap<string, FirmwareMonitoringSample>,
  current: readonly FirmwareMonitoringSample[],
): string[] {
  return current.filter(sample => isArrival(previous.get(sample.sensor_id), sample)).map(sample => sample.sensor_id);
}

/** UI estimate only. Never mutates the API payload or its freshness policy. */
export function effectiveSampleAge(ageMs: number | null, elapsedMs: number): number | null {
  if (typeof ageMs !== 'number' || !Number.isFinite(ageMs) || ageMs < 0) return null;
  return ageMs + (Number.isFinite(elapsedMs) ? Math.max(0, elapsedMs) : 0);
}

export class MonitoringAgeTracker {
  private anchors = new Map<string, { identity: string; age: number | null; at: number }>();
  clear() { this.anchors.clear(); }
  observe(samples: readonly FirmwareMonitoringSample[], nowMs: number): FirmwareMonitoringSample[] {
    const next = new Map<string, { identity: string; age: number | null; at: number }>();
    const aged = samples.map(sample => {
      const identity = captureIdentity(sample);
      const prior = this.anchors.get(sample.sensor_id);
      let age = effectiveSampleAge(sample.age_ms, 0);
      if (age !== null && prior?.identity === identity && prior.age !== null) {
        age = Math.max(age, effectiveSampleAge(prior.age, nowMs - prior.at)!);
      }
      next.set(sample.sensor_id, { identity, age, at: nowMs });
      return { ...sample, age_ms: age };
    });
    this.anchors = next;
    return aged;
  }
}
