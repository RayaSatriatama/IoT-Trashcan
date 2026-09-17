import { describe, expect, it, vi } from 'vitest';

vi.mock('expo-router', () => ({
  Link: ({ children }: { children: unknown }) => children,
  useRouter: () => ({ push: vi.fn() }),
}));

import { LOCATION_STATUS_FILTERS, SEARCH_INPUT_LABEL } from '../../app/locations';

describe('locations route filter contracts', () => {
  it('exposes an accessible search label', () => {
    expect(SEARCH_INPUT_LABEL).toBe('Cari lokasi');
  });

  it('uses only the canonical API status values', () => {
    expect(LOCATION_STATUS_FILTERS.map((filter) => filter.value)).toEqual([
      '',
      'Normal',
      'Waspada',
      'Angkat',
      'Safety Alert',
      'Offline',
      'Data Invalid',
    ]);
  });
});
