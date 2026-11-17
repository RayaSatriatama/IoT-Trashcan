import type { SensorHistoryPoint } from '../api/types';
import { SENSOR_METRICS, sensorMetricValue } from './sensorHistory';
import { estimateNh3FromPinMv } from './mq135Nh3Estimate';

/**
 * CSV export of the history the page has already loaded.
 *
 * One row per sample. Columns cover the record metadata, every engineering metric
 * of that sensor, and every raw field the server published, so a sensor whose
 * converted value is missing still contributes its raw measurement. Nothing is
 * recomputed and no sample is invented.
 */

const escapeCell = (value: string): string => {
  if (/[",\n;]/.test(value)) return `"${value.replace(/"/g, '""')}"`;
  return value;
};

const cell = (value: number | string | null | undefined): string => {
  if (value === null || value === undefined) return '';
  if (typeof value === 'number') return Number.isFinite(value) ? String(value) : '';
  return escapeCell(value);
};

export function buildHistoryCsv(bySensor: Readonly<Record<string, readonly SensorHistoryPoint[]>>): string {
  const sensors = Object.keys(bySensor).filter(id => (bySensor[id] ?? []).length > 0).sort();
  const metricColumns = sensors.flatMap(id => (SENSOR_METRICS[id] ?? []).map(metric => ({ id, metric })));
  const rawKeys = [...new Set(sensors.flatMap(id => (bySensor[id] ?? []).flatMap(point => Object.keys(point.raw ?? {}))))].sort();
  const header = [
    'sensor_id', 'recorded_at', 'time_quality', 'quality', 'reason', 'sample_origin',
    'boot_id', 'sequence', 'monotonic_us',
    ...metricColumns.map(({ metric }) => `${metric.key}${metric.unit ? ` (${metric.unit})` : ''}`),
    // MQ-135 NH3 is derived from the calibrated pin voltage by the same estimator
    // the UI uses, so the file matches what the page shows.
    'nh3_ppm (ppm)', 'nh3_status',
    ...rawKeys.map(key => `raw_${key}`),
  ];
  const lines = [header.join(',')];
  for (const id of sensors) {
    for (const point of bySensor[id] ?? []) {
      const row = [
        escapeCell(id),
        point.recorded_at ?? '',
        point.time_quality ?? '',
        point.quality ?? '',
        escapeCell(point.reason ?? ''),
        point.sample_origin ?? '',
        point.boot_id ?? '',
        point.sequence ?? '',
        point.monotonic_us ?? '',
      ];
      for (const { id: columnSensor, metric } of metricColumns) {
        row.push(columnSensor === id ? cell(sensorMetricValue(point.converted, metric)) : '');
      }
      const nh3 = id === 'mq135' ? estimateNh3FromPinMv(point.converted?.adc_pin_mv ?? point.converted?.adc_mv) : null;
      row.push(nh3 && nh3.ppm !== null ? cell(Math.round(nh3.ppm * 1000) / 1000) : '');
      row.push(nh3 ? nh3.status : '');
      for (const key of rawKeys) row.push(cell(point.raw?.[key]));
      lines.push(row.join(','));
    }
  }
  return `${lines.join('\n')}\n`;
}

/** Triggers a browser download. Returns false outside a DOM environment. */
export function downloadCsv(filename: string, text: string): boolean {
  if (typeof document === 'undefined' || typeof URL === 'undefined') return false;
  const blob = new Blob([text], { type: 'text/csv;charset=utf-8;' });
  const url = URL.createObjectURL(blob);
  const anchor = document.createElement('a');
  anchor.href = url;
  anchor.download = filename;
  anchor.style.display = 'none';
  document.body.appendChild(anchor);
  anchor.click();
  document.body.removeChild(anchor);
  setTimeout(() => URL.revokeObjectURL(url), 1000);
  return true;
}

export function csvFilename(deviceId: string, at = new Date()): string {
  const stamp = at.toISOString().replace(/[:.]/g, '-').slice(0, 19);
  const safe = deviceId.replace(/[^a-zA-Z0-9._-]/g, '_') || 'device';
  return `sigap-${safe}-${stamp}.csv`;
}
