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
  HISTORY_STATUS_FILTERS,
  HISTORY_TABLE_COLUMNS,
  historyRangeLabel,
  serializeHistoryFilters,
  totalHistoryPages,
  formatHistoryScore,
  validateHistoryPeriod,
} from '../../app/history';
import { trendBucketAccessibilityLabel, trendBucketTotal } from '../../components/history/HistoryAnalytics';
import {
  appliedHistoryDraft,
  type HistoryFilterDraft,
} from '../../components/history/HistoryFilters';

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

  it('converts complete date-only bounds from Asia/Jakarta to UTC inclusive/exclusive boundaries', () => {
    expect(dateInputToUtcRange('2026-09-10', '2026-09-12')).toEqual({
      from: '2026-09-09T17:00:00.000Z',
      to: '2026-09-12T17:00:00.000Z',
    });
  });

  it('preserves partial API date serialization while the UI validates draft dates separately', () => {
    expect(dateInputToUtcRange('', '')).toEqual({ from: undefined, to: undefined });
    expect(dateInputToUtcRange('2026-09-10', '')).toEqual({ from: '2026-09-09T17:00:00.000Z', to: undefined });
  });

  it('validates the complete History date period required by the UI draft', () => {
    expect(validateHistoryPeriod('', '')).toBeNull();
    expect(validateHistoryPeriod('2026-09-10', '')).toBe('Lengkapi tanggal awal dan akhir.');
    expect(validateHistoryPeriod('', '2026-09-12')).toBe('Lengkapi tanggal awal dan akhir.');
    expect(validateHistoryPeriod('2026-09-12', '2026-09-10')).toBe('Tanggal awal tidak boleh melewati tanggal akhir.');
    expect(validateHistoryPeriod('2026-09-10', '2026-09-12')).toBeNull();
  });

  it('shows Safety Alert as the effective status when safety overrides FIS', () => {
    expect(effectiveHistoryStatus({ fis_status: 'Angkat', safety_state: 'Safety Alert' })).toBe('Safety Alert');
    expect(effectiveHistoryStatus({ fis_status: 'Waspada', safety_state: 'Clear' })).toBe('Waspada');
    expect(effectiveHistoryStatus({ fis_status: null, safety_state: null })).toBeNull();
  });

  it('does not expose Offline until historical offline semantics exist', () => {
    expect(HISTORY_STATUS_FILTERS.some((filter) => filter.value === 'Offline')).toBe(false);
  });

  it('projects server trend buckets into accessible totals', () => {
    const bucket = {
      start: '2026-09-23', label: '23 Sep', total: 10,
      normal: 5, waspada: 2, pickup: 1, safety_alert: 1, data_invalid: 1,
    };
    expect(trendBucketTotal(bucket)).toBe(10);
    expect(trendBucketAccessibilityLabel(bucket)).toContain('Normal 5');
    expect(trendBucketAccessibilityLabel(bucket)).toContain('Data tidak valid 1');
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

  it('commits a draft as trimmed API filters only after a valid period', () => {
    const draft: HistoryFilterDraft = {
      fromDate: '2026-09-10',
      toDate: '2026-09-12',
      location_id: 'loc-1',
      device_id: '  SIM-CANTEEN-01  ',
      status: 'Waspada',
    };

    expect(appliedHistoryDraft(draft, dateInputToUtcRange(draft.fromDate, draft.toDate))).toEqual({
      from: '2026-09-09T17:00:00.000Z',
      to: '2026-09-12T17:00:00.000Z',
      location_id: 'loc-1',
      device_id: 'SIM-CANTEEN-01',
      status: 'Waspada',
    });
  });

  it('omits whitespace-only device queries from committed filters', () => {
    const draft: HistoryFilterDraft = {
      fromDate: '',
      toDate: '',
      device_id: '   ',
    };

    expect(appliedHistoryDraft(draft, dateInputToUtcRange('', ''))).toEqual({
      from: undefined,
      to: undefined,
      location_id: undefined,
      device_id: undefined,
      status: undefined,
    });
  });
});
