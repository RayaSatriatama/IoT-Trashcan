import type {
  DeviceDecisionSummaryDTO,
  FuzzyTraceDTO,
  ResearchSlotDTO,
  SensorReadingDTO,
  SensorState,
  SlotDTO,
} from '../api/types';
import { formatNullableMetric } from '../format';
import type { MetricBarTone } from '../../components/MetricBar';

export interface SlotMetricPresentation {
  label: string;
  value: string;
  accessibilityLabel: string;
  numericValue?: number | null;
  unit?: string;
}

export interface DecisionInputPresentation {
  key: 'fill' | 'weight_utilization' | 'ogi' | 'accumulation_hours';
  label: string;
  value: string;
  category: string;
}

export type SensorRole = 'Fuzzy Fill' | 'Fuzzy Weight' | 'OGI source' | 'OGI supporting' | 'Safety' | 'Temperature safety' | 'Derived parameter';

export interface ResearchSensorPresentation {
  id: string;
  label: string;
  parameter: string;
  actual: string;
  raw: string;
  normalized: string;
  normalizedLabel: 'Normalized' | 'Input FIS';
  state: SensorState;
  role: SensorRole;
  membership: string;
}

const streamLabels: Record<string, string> = {
  organic: 'Organik',
  inorganic: 'Anorganik',
  b3: 'B3',
};

const sensorLabels: Record<string, string> = {
  fill: 'Fill Sensor',
  fill_sensor: 'Fill Sensor',
  hx711: 'HX711 / Load Cell',
  load_cell: 'HX711 / Load Cell',
  mq135: 'MQ-135',
  mq_135: 'MQ-135',
  mq4: 'MQ-4',
  mq_4: 'MQ-4',
  mq7: 'MQ-7',
  mq_7: 'MQ-7',
  mq8: 'MQ-8',
  mq_8: 'MQ-8',
  ds18b20: 'DS18B20',
};

const sensorParameterLabels: Record<string, string> = {
  fill: 'Keterisian',
  weight: 'Berat',
  broad_gas: 'Broad gas response',
  ch4: 'CH4 response',
  co: 'CO response',
  h2: 'H2 response',
  temperature: 'Suhu',
};

const relatedSensors: Record<string, string[]> = {
  organic: ['Fill Sensor', 'HX711', 'MQ-135', 'MQ-4'],
  inorganic: ['Fill Sensor', 'HX711', 'MQ-135'],
  b3: ['Fill Sensor', 'HX711', 'MQ-135', 'MQ-7', 'MQ-8', 'DS18B20'],
};

export function getStreamLabel(stream: string): string {
  return streamLabels[stream] ?? stream;
}

export function getSensorLabel(sensor: Pick<SensorReadingDTO, 'sensor_id' | 'sensor_type'>): string {
  const key = `${sensor.sensor_type || sensor.sensor_id}`.toLowerCase().replace(/[^a-z0-9]/g, '_');
  return sensorLabels[key] ?? (sensor.sensor_type || sensor.sensor_id);
}

export function getSensorParameterLabel(parameter: string): string {
  return sensorParameterLabels[parameter.toLowerCase()] ?? parameter;
}

export function getRelatedSensorLabels(stream: string): string[] {
  return relatedSensors[stream] ?? [];
}

export function formatSensorState(state: SensorState | string | null | undefined): string {
  switch (state) {
    case 'VALID': return 'Valid';
    case 'WARMING_UP': return 'Warm-up';
    case 'STALE': return 'Stale';
    case 'INVALID': return 'Invalid';
    case 'DISABLED': return 'Disabled';
    default: return 'Belum tersedia';
  }
}

const membershipLabels: Record<string, string> = {
  low: 'Rendah',
  medium: 'Sedang',
  high: 'Tinggi',
};

const membershipOrder: Record<string, number> = {
  low: 0,
  medium: 1,
  high: 2,
};

function activeMemberships(memberships: Record<string, number> | null | undefined) {
  if (!memberships) return '—';
  return Object.entries(memberships)
    .filter(([, value]) => Number.isFinite(value) && value > 0)
    .map(([label, value]) => ({
      label,
      normalized: label.toLowerCase(),
      value,
    }));
}

function membershipLabel(label: string): string {
  return membershipLabels[label.toLowerCase()] ?? label;
}

export function formatMembershipCategory(memberships: Record<string, number> | null | undefined): string {
  const entries = activeMemberships(memberships);
  if (entries === '—' || !entries.length) return '—';
  const strongest = [...entries].sort((a, b) => b.value - a.value)[0];
  return membershipLabel(strongest.label);
}

export function formatMembership(memberships: Record<string, number> | null | undefined): string {
  const entries = activeMemberships(memberships);
  if (entries === '—' || !entries.length) return '—';
  return [...entries]
    .sort((a, b) => (membershipOrder[a.normalized] ?? 99) - (membershipOrder[b.normalized] ?? 99))
    .map(({ label, value }) => `${membershipLabel(label)} ${value.toFixed(2).replace('.', ',')}`)
    .join(' · ');
}

export function formatResearchNumber(value: number | null | undefined, unit = ''): string {
  if (value === null || value === undefined || !Number.isFinite(value)) return '—';
  const rounded = Number.isInteger(value) ? String(value) : value.toFixed(2).replace(/0+$/, '').replace(/\.$/, '');
  return `${rounded}${unit}`;
}

export function roleForSensor(sensor: Pick<SensorReadingDTO, 'sensor_type' | 'sensor_id' | 'parameter'>): SensorRole {
  const key = `${sensor.sensor_type} ${sensor.sensor_id} ${sensor.parameter}`.toLowerCase();
  if (key.includes('fill')) return 'Fuzzy Fill';
  if (key.includes('hx711') || key.includes('load')) return 'Fuzzy Weight';
  if (key.includes('mq-7') || key.includes('mq7') || key.includes('mq-8') || key.includes('mq8')) return 'Safety';
  if (key.includes('ds18')) return 'Temperature safety';
  if (key.includes('mq-4') || key.includes('mq4')) return 'OGI supporting';
  if (key.includes('mq-135') || key.includes('mq135')) return 'OGI source';
  return 'Derived parameter';
}

export function buildResearchSensorRows(slot: ResearchSlotDTO): ResearchSensorPresentation[] {
  return (slot.sensors ?? []).map((sensor) => {
    const role = roleForSensor(sensor);
    const membershipKey = role === 'Fuzzy Fill' ? 'fill' : role === 'Fuzzy Weight' ? 'weight_utilization' : role.startsWith('OGI') ? 'ogi' : 'safety';
    const membership = role === 'Safety' || role === 'Temperature safety'
      ? 'Safety'
      : formatMembership(slot.fis_trace?.memberships?.[membershipKey]);
    const actualUnit = sensor.unit ? ` ${sensor.unit}` : '';
    const rawUnit = sensor.raw_unit ? ` ${sensor.raw_unit}` : '';
    return {
      id: sensor.sensor_id,
      label: getSensorLabel(sensor),
      parameter: getSensorParameterLabel(sensor.parameter),
      actual: formatResearchNumber(sensor.engineering_value, actualUnit),
      raw: formatResearchNumber(sensor.raw_value, rawUnit),
      normalized: formatResearchNumber(sensor.normalized_value),
      normalizedLabel: 'Normalized',
      state: sensor.sensor_state,
      role,
      membership,
    };
  });
}

export function buildDerivedResearchRows(slot: ResearchSlotDTO): ResearchSensorPresentation[] {
  const derived = slot.derived_parameters;
  const trace = slot.fis_trace;
  return [
    {
      id: 'derived-ogi', label: 'Derived OGI', parameter: 'Gas index',
      actual: formatResearchNumber(derived.ogi), raw: '—', normalized: formatResearchNumber(trace?.inputs?.ogi),
      normalizedLabel: 'Input FIS',
      state: 'VALID', role: 'Derived parameter', membership: formatMembership(trace?.memberships?.ogi),
    },
    {
      id: 'derived-time', label: 'Accumulation Time', parameter: 'Durasi akumulasi',
      actual: formatResearchNumber(derived.accumulation_hours, ' jam'), raw: '—', normalized: formatResearchNumber(trace?.inputs?.accumulation_hours, ' jam'),
      normalizedLabel: 'Input FIS',
      state: 'VALID', role: 'Derived parameter', membership: formatMembership(trace?.memberships?.accumulation_hours),
    },
  ];
}

export function buildDecisionInputRows(slot: SlotDTO, research?: ResearchSlotDTO | null): DecisionInputPresentation[] {
  const trace = research?.fis_trace;
  const ogi = research?.derived_parameters.ogi ?? research?.normalized.ogi ?? slot.ogi;
  const accumulation = research?.derived_parameters.accumulation_hours ?? research?.normalized.accumulation_hours ?? slot.accumulation_hours;
  const definitions: Array<[DecisionInputPresentation['key'], string, number | null | undefined, string]> = [
    ['fill', 'Keterisian', trace?.inputs?.fill ?? slot.fill, '%'],
    ['weight_utilization', 'Utilisasi berat', trace?.inputs?.weight_utilization ?? slot.weight_utilization, '%'],
    ['ogi', 'Indeks gas', trace?.inputs?.ogi ?? ogi, ''],
    ['accumulation_hours', 'Waktu akumulasi', trace?.inputs?.accumulation_hours ?? accumulation, ' jam'],
  ];
  return definitions.map(([key, label, value, unit]) => ({
    key,
    label,
    value: formatResearchNumber(value, unit),
    category: formatMembershipCategory(trace?.memberships?.[key]),
  }));
}

export function formatDecisionSummary(summary: DeviceDecisionSummaryDTO | null | undefined) {
  const mode = summary?.mode === 'Fuzzy multi-parameter' ? 'Penilaian multi-parameter' : summary?.mode;
  return {
    score: formatResearchNumber(summary?.priority_score, ' / 100'),
    criticalStream: summary?.critical_stream ? getStreamLabel(summary.critical_stream) : '—',
    reason: summary?.critical_reason || '—',
    mode: mode || 'Belum tersedia',
  };
}

export function getTraceInputRows(trace: FuzzyTraceDTO | null | undefined) {
  if (!trace) return [];
  const labels: Array<[string, string, string]> = [
    ['fill', 'Keterisian', '%'], ['weight_utilization', 'Utilisasi berat', '%'], ['ogi', 'OGI', ''], ['accumulation_hours', 'Waktu akumulasi', ' jam'],
  ];
  return labels.map(([key, label, unit]) => ({
    key,
    label,
    value: formatResearchNumber(trace.inputs?.[key], unit),
    membership: formatMembership(trace.memberships?.[key]),
  }));
}

export function getStreamTone(stream: string): MetricBarTone {
  if (stream === 'organic') return 'green';
  if (stream === 'inorganic') return 'blue';
  if (stream === 'b3') return 'red';
  return 'neutral';
}

export function formatNullableDisplay(value: number | null | undefined, suffix = ''): string {
  if (value === null || value === undefined || !Number.isFinite(value)) return '—';
  return formatNullableMetric(value, suffix);
}

export function formatSafetyState(value: string | null | undefined): string {
  if (value === null || value === undefined || value === '') return '—';
  if (value === 'Clear') return 'Aman';
  if (value === 'Unavailable') return 'Belum tersedia';
  return value;
}

export function formatShortTime(value: string | null | undefined): string {
  if (!value) return '—';
  const date = new Date(value);
  if (Number.isNaN(date.getTime())) return '—';
  return new Intl.DateTimeFormat('id-ID', {
    hour: '2-digit',
    minute: '2-digit',
    hour12: false,
    timeZone: 'Asia/Jakarta',
  }).format(date).replace('.', ':');
}

export function formatResearchTimestamp(value: string | null | undefined): string {
  if (!value) return '—';
  const date = new Date(value);
  if (Number.isNaN(date.getTime())) return '—';
  return new Intl.DateTimeFormat('id-ID', {
    day: '2-digit',
    month: 'short',
    year: 'numeric',
    hour: '2-digit',
    minute: '2-digit',
  }).format(date);
}

export function buildSlotMetrics(slot: SlotDTO): SlotMetricPresentation[] {
  const metrics: SlotMetricPresentation[] = [
    {
      label: 'Berat',
      value: formatNullableDisplay(slot.weight, ' kg'),
      accessibilityLabel: 'Berat',
      numericValue: slot.weight,
      unit: ' kg',
    },
  ];

  if (slot.stream === 'b3') {
    metrics.push(
      {
        label: 'Suhu',
        value: formatNullableDisplay(slot.temperature, ' °C'),
        accessibilityLabel: 'Suhu',
        numericValue: slot.temperature,
        unit: ' °C',
      },
      {
        label: 'Keselamatan',
        value: formatSafetyState(slot.safety_state),
        accessibilityLabel: 'Keselamatan',
      },
    );
  } else {
    metrics.push(
      {
        label: 'OGI',
        value: formatNullableDisplay(slot.ogi),
        accessibilityLabel: 'OGI',
        numericValue: slot.ogi,
        unit: '',
      },
      {
        label: 'Update',
        value: formatShortTime(slot.recorded_at),
        accessibilityLabel: 'Waktu pembaruan',
      },
    );
  }

  return metrics;
}

export function shouldShowDecisionNote(slot: Pick<SlotDTO, 'status' | 'dominant_reason'>): boolean {
  return Boolean(slot.dominant_reason && slot.status && slot.status !== 'Normal');
}

export function shouldShowFinalStatus(decisionStatus: string | null | undefined, operationalStatus: string | null | undefined): boolean {
  return Boolean(operationalStatus && operationalStatus !== decisionStatus);
}
