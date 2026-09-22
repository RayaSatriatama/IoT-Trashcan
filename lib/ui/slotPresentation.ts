import type { SlotDTO } from '../api/types';
import { formatNullableMetric } from '../format';
import type { MetricBarTone } from '../../components/MetricBar';

export interface SlotMetricPresentation {
  label: string;
  value: string;
  accessibilityLabel: string;
  numericValue?: number | null;
  unit?: string;
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
    timeZone: 'UTC',
  }).format(date).replace('.', ':');
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
