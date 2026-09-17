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
  HISTORY_STATUS_FILTERS,
  serializeHistoryFilters,
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
});
