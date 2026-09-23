import { describe, expect, it, vi } from 'vitest';

vi.mock('expo-router', () => ({
  useRouter: () => ({ push: vi.fn() }),
}));

vi.mock('react-native', () => ({
  Image: () => null,
}));

import {
  buildHistoryExportHref,
  dateInputToUtcRange,
  effectiveHistoryStatus,
  formatHistoryCount,
  formatHistoryTimestamp,
  groupHistoryByDay,
  HISTORY_STATUS_FILTERS,
  HISTORY_TABLE_COLUMNS,
  historyRangeLabel,
  serializeHistoryFilters,
  totalHistoryPages,
  formatHistoryScore,
} from '../../app/history';

describe('history filter contracts', () => {
  it('serializes all filters with the API query names', () => {
    expect(serializeHistoryFilters({
      from: '2026-09-01T00:00:00.000Z',
      to: '2026-09-18T00:00:00.000Z',
      location_id: 'loc/1',
      device_id: 'SIGAP 01',
      status: 'Safety Alert',
    })).toBe('from=2026-09-01T00%3A00%3A00.000Z&to=2026-09-18T00%3A00%3A00.000Z&location_id=loc%2F1&device_id=SIGAP+01&status=Safety+Alert');
  });

  it('omits empty filters and produces the same query for export', () => {
    const filters = { location_id: 'loc-1', status: 'Waspada' as const };
    expect(serializeHistoryFilters(filters)).toBe('location_id=loc-1&status=Waspada');
    expect(buildHistoryExportHref(filters)).toBe('/api/export.csv?location_id=loc-1&status=Waspada');
  });

  it('converts date-only bounds to UTC inclusive/exclusive boundaries', () => {
    expect(dateInputToUtcRange('2026-09-10', '2026-09-12')).toEqual({
      from: '2026-09-10T00:00:00.000Z',
      to: '2026-09-13T00:00:00.000Z',
    });
  });

  it('does not create a range when no date is selected', () => {
    expect(dateInputToUtcRange('', '')).toEqual({ from: undefined, to: undefined });
    expect(dateInputToUtcRange('2026-09-10', '')).toEqual({ from: '2026-09-10T00:00:00.000Z', to: undefined });
  });

  it('shows Safety Alert as the effective status when safety overrides FIS', () => {
    expect(effectiveHistoryStatus({ fis_status: 'Angkat', safety_state: 'Safety Alert' })).toBe('Safety Alert');
    expect(effectiveHistoryStatus({ fis_status: 'Waspada', safety_state: 'Clear' })).toBe('Waspada');
    expect(effectiveHistoryStatus({ fis_status: null, safety_state: null })).toBeNull();
  });

  it('does not expose Offline until historical offline semantics exist', () => {
    expect(HISTORY_STATUS_FILTERS.some((filter) => filter.value === 'Offline')).toBe(false);
  });

  it('groups daily history into stacked trend status counts', () => {
    const items = [
      { recorded_at: '2026-09-02T10:00:00Z', fis_status: 'Normal', safety_state: 'Clear' },
      { recorded_at: '2026-09-02T11:00:00Z', fis_status: 'Angkat', safety_state: 'Clear' },
      { recorded_at: '2026-09-03T11:00:00Z', fis_status: 'Waspada', safety_state: 'Safety Alert' },
    ] as never[];
    expect(groupHistoryByDay(items)).toEqual([
      { day: '2026-09-02', normal: 1, pickup: 1, safety: 0 },
      { day: '2026-09-03', normal: 0, pickup: 0, safety: 1 },
    ]);
  });

  it('computes bounded page counts and formats audit scores', () => {
    expect(totalHistoryPages(0, 10)).toBe(1);
    expect(totalHistoryPages(21, 10)).toBe(3);
    expect(totalHistoryPages(21, 20)).toBe(2);
    expect(formatHistoryScore(13.33333333333332)).toBe('13.33');
    expect(formatHistoryScore(null)).toBe('—');
    expect(formatHistoryCount(4689)).toBe('4.689');
    expect(historyRangeLabel(1, 10, 4689)).toBe('1–10 dari 4.689 data');
    expect(formatHistoryTimestamp('2026-09-18T06:07:00Z')).toEqual({ time: '13:07', date: '18 Sep 2026' });
  });

  it('keeps device identity and placement separate within the device cell', () => {
    expect(HISTORY_TABLE_COLUMNS).toEqual([
      'PERANGKAT',
      'LOKASI',
      'STREAM',
      'STATUS',
      'KETERISIAN',
      'BERAT',
      'WAKTU',
      'AKSI',
    ]);
  });
});
