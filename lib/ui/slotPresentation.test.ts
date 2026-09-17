import { describe, expect, it } from 'vitest';

import type { SlotDTO } from '../api/types';
import {
  buildSlotMetrics,
  formatNullableDisplay,
  formatSafetyState,
  getStreamTone,
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
    expect(buildSlotMetrics(baseSlot).map((metric) => [metric.label, metric.value])).toEqual([
      ['Berat', '5 kg'],
      ['OGI', '—'],
      ['Update', '00:04'],
    ]);

    expect(buildSlotMetrics({ ...baseSlot, stream: 'b3', temperature: 32, safety_state: 'Clear' }).map((metric) => [metric.label, metric.value])).toEqual([
      ['Berat', '5 kg'],
      ['Suhu', '32 °C'],
      ['Keselamatan', 'Aman'],
    ]);
  });

  it('only exposes decision notes for meaningful non-normal states', () => {
    expect(shouldShowDecisionNote(baseSlot)).toBe(false);
    expect(shouldShowDecisionNote({ ...baseSlot, status: 'Waspada', dominant_reason: 'needs attention' })).toBe(true);
    expect(shouldShowDecisionNote({ ...baseSlot, status: 'Waspada', dominant_reason: null })).toBe(false);
  });
});
