import { describe, expect, it } from 'vitest';

import type { SlotDTO } from '../api/types';
import {
  buildDerivedResearchRows,
  buildResearchSensorRows,
  buildSlotMetrics,
  formatDecisionSummary,
  formatMembership,
  formatResearchTimestamp,
  formatSensorState,
  formatNullableDisplay,
  formatSafetyState,
  getStreamTone,
  getRelatedSensorLabels,
  shouldShowDecisionNote,
} from './slotPresentation';

const baseSlot: SlotDTO = {
  stream: 'organic',
  fill: 25,
  weight: 5,
  weight_utilization: 25,
  status: 'Normal',
  dominant_reason: null,
  safety_state: null,
  recorded_at: '2026-09-18T00:04:00Z',
  ogi: null,
  accumulation_hours: null,
  temperature: null,
};

describe('slot presentation helpers', () => {
  it('keeps physical sensor rows distinct from derived research values', () => {
    const rows = buildResearchSensorRows({
      stream: 'organic', recorded_at: null,
      raw: { fill: 53, weight: 5, gas_raw: 1834, temperature: null },
      normalized: { fill_pct: 53, weight_utilization: 45, ogi: 71, accumulation_hours: 13, temperature: null },
      sensor_health: null, fis_score: 62, fis_status: 'Waspada', fis_version: 'organic-v2', dominant_reason: null, safety_state: null,
      sensors: [
        { sensor_id: 'fill-1', sensor_type: 'fill', parameter: 'fill', raw_value: 12, engineering_value: 53, normalized_value: 0.53, raw_unit: 'cm', unit: '%', sensor_state: 'VALID' },
        { sensor_id: 'mq135-1', sensor_type: 'MQ-135', parameter: 'broad_gas', raw_value: 1834, engineering_value: null, normalized_value: 0.71, raw_unit: 'ADC', unit: '', sensor_state: 'VALID' },
      ],
      derived_parameters: { ogi: 71, accumulation_hours: 13 },
      fis_trace: { inputs: { fill: 53, weight_utilization: 45, ogi: 71, accumulation_hours: 13 }, memberships: { fill: { medium: 0.8 }, ogi: { high: 0.9 } }, rules: null, triggered_rules: 2, score: 62, status: 'Waspada', version: 'organic-v2', dominant_reason: 'gas' },
      safety: null,
    });
    expect(rows.map((row) => row.label)).toEqual(['Fill Sensor', 'MQ-135']);
    expect(rows[0].role).toBe('Fuzzy Fill');
    expect(rows[0].raw).toBe('12 cm');
    expect(rows[1].role).toBe('OGI source');
    expect(rows[1].raw).toBe('1834 ADC');
    const derivedRows = buildDerivedResearchRows({
      stream: 'organic', recorded_at: null, raw: { fill: null, weight: null, gas_raw: null, temperature: null }, normalized: { fill_pct: null, weight_utilization: null, ogi: null, accumulation_hours: null, temperature: null }, sensor_health: null, fis_score: null, fis_status: null, fis_version: null, dominant_reason: null, safety_state: null, sensors: [], derived_parameters: { ogi: null, accumulation_hours: null }, fis_trace: null, safety: null,
    });
    expect(derivedRows.map((row) => row.label)).toEqual(['Derived OGI', 'Accumulation Time']);
    expect(derivedRows.every((row) => row.normalizedLabel === 'Input FIS')).toBe(true);
  });

  it('uses readable state and membership labels without inventing ppm', () => {
    expect(formatSensorState('WARMING_UP')).toBe('Warm-up');
    expect(formatMembership({ low: 0.2, high: 0.8 })).toBe('Tinggi (80%)');
    expect(formatMembership(null)).toBe('—');
  });

  it('formats decision summary as unavailable when API evidence is absent', () => {
    expect(formatDecisionSummary(null)).toEqual({ score: '—', criticalStream: '—', reason: '—', mode: 'Belum tersedia' });
  });

  it('keeps related physical-sensor metadata centralized per stream', () => {
    expect(getRelatedSensorLabels('organic')).toEqual(['Fill Sensor', 'HX711', 'MQ-135', 'MQ-4']);
    expect(getRelatedSensorLabels('b3')).toEqual(['Fill Sensor', 'HX711', 'MQ-135', 'MQ-7', 'MQ-8', 'DS18B20']);
  });

  it('formats research timestamps for operators instead of exposing RFC3339', () => {
    expect(formatResearchTimestamp('2026-09-23T02:00:00Z')).not.toContain('T02:00:00');
    expect(formatResearchTimestamp(null)).toBe('—');
  });
  it('maps stream identity to semantic metric tones', () => {
    expect(getStreamTone('organic')).toBe('green');
    expect(getStreamTone('inorganic')).toBe('blue');
    expect(getStreamTone('b3')).toBe('red');
    expect(getStreamTone('unknown')).toBe('neutral');
  });

  it('uses a compact dash for unavailable numeric data', () => {
    expect(formatNullableDisplay(null)).toBe('—');
    expect(formatNullableDisplay(undefined, ' kg')).toBe('—');
    expect(formatNullableDisplay(5, ' kg')).toBe('5 kg');
  });

  it('translates safety presentation without changing API semantics', () => {
    expect(formatSafetyState('Clear')).toBe('Aman');
    expect(formatSafetyState('Unavailable')).toBe('Belum tersedia');
    expect(formatSafetyState(null)).toBe('—');
    expect(formatSafetyState('Safety Alert')).toBe('Safety Alert');
  });

  it('builds stream-specific footer metrics', () => {
    expect(buildSlotMetrics(baseSlot).map((metric) => [metric.label, metric.value, metric.numericValue, metric.unit])).toEqual([
      ['Berat', '5 kg', 5, ' kg'],
      ['OGI', '—', null, ''],
      ['Update', '07:04', undefined, undefined],
    ]);

    expect(buildSlotMetrics({ ...baseSlot, stream: 'b3', temperature: 32, safety_state: 'Clear' }).map((metric) => [metric.label, metric.value, metric.numericValue, metric.unit])).toEqual([
      ['Berat', '5 kg', 5, ' kg'],
      ['Suhu', '32 °C', 32, ' °C'],
      ['Keselamatan', 'Aman', undefined, undefined],
    ]);
  });

  it('only exposes decision notes for meaningful non-normal states', () => {
    expect(shouldShowDecisionNote(baseSlot)).toBe(false);
    expect(shouldShowDecisionNote({ ...baseSlot, status: 'Waspada', dominant_reason: 'needs attention' })).toBe(true);
    expect(shouldShowDecisionNote({ ...baseSlot, status: 'Waspada', dominant_reason: null })).toBe(false);
  });
});
