import { describe, expect, it } from 'vitest';

import {
  RESEARCH_UNAVAILABLE_COPY,
  formatResearchValue,
  formatSensorHealth,
  getResearchPanelState,
} from '../../components/ResearchPanel';

describe('research panel value presentation', () => {
  it('uses explicit unavailable copy for null values', () => {
    expect(formatResearchValue(null)).toBe(RESEARCH_UNAVAILABLE_COPY);
    expect(formatResearchValue(undefined)).toBe(RESEARCH_UNAVAILABLE_COPY);
    expect(formatResearchValue(Number.NaN)).toBe(RESEARCH_UNAVAILABLE_COPY);
  });

  it('preserves raw field labels and optional units without inventing values', () => {
    expect(formatResearchValue(0, '%')).toBe('0 %');
    expect(formatResearchValue(4.5, 'kg')).toBe('4.5 kg');
    expect(formatResearchValue('gas_raw')).toBe('gas_raw');
  });

  it('serializes sensor health safely', () => {
    expect(formatSensorHealth(null)).toBe(RESEARCH_UNAVAILABLE_COPY);
    expect(formatSensorHealth({ status: 'ok' })).toBe('{"status":"ok"}');
    expect(formatSensorHealth(false)).toBe('false');
  });
});

describe('research panel states', () => {
  it('prioritizes loading and error before empty/data', () => {
    const data = { device_id: 'device-1', decision_summary: { priority_score: null, critical_stream: null, critical_reason: null, mode: null }, slots: [{ stream: 'organic' }] } as never;
    expect(getResearchPanelState({ data, loading: true, error: new Error('late') })).toBe('loading');
    expect(getResearchPanelState({ data: null, error: new Error('offline') })).toBe('error');
  });

  it('distinguishes empty research from loaded slot data', () => {
    expect(getResearchPanelState({ data: null })).toBe('empty');
    expect(getResearchPanelState({ data: { device_id: 'device-1', decision_summary: { priority_score: null, critical_stream: null, critical_reason: null, mode: null }, slots: [] } })).toBe('empty');
    expect(
      getResearchPanelState({
        data: { device_id: 'device-1', decision_summary: { priority_score: null, critical_stream: null, critical_reason: null, mode: null }, slots: [{ stream: 'organic' }] } as never,
      }),
    ).toBe('data');
  });
});
