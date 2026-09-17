import { describe, expect, it } from 'vitest';

import { getStatusPresentation, isCanonicalStatus } from './status';
import { formatNullableMetric } from './format';

describe('status presentation', () => {
  it('maps every canonical API status to an Indonesian label and stable token', () => {
    expect(getStatusPresentation('Normal')).toEqual({ label: 'Normal', token: 'statusNormal' });
    expect(getStatusPresentation('Waspada')).toEqual({ label: 'Waspada', token: 'statusWaspada' });
    expect(getStatusPresentation('Angkat')).toEqual({ label: 'Perlu diangkat', token: 'statusAngkat' });
    expect(getStatusPresentation('Safety Alert')).toEqual({
      label: 'Peringatan keselamatan',
      token: 'statusSafetyAlert',
    });
    expect(getStatusPresentation('Offline')).toEqual({ label: 'Offline', token: 'statusOffline' });
    expect(getStatusPresentation('Data Invalid')).toEqual({
      label: 'Data tidak valid',
      token: 'statusDataInvalid',
    });
  });

  it('recognizes only canonical status values', () => {
    expect(isCanonicalStatus('Safety Alert')).toBe(true);
    expect(isCanonicalStatus('Unknown')).toBe(false);
  });

  it('keeps unavailable metrics explicit instead of converting null to zero', () => {
    expect(formatNullableMetric(null)).toBe('Belum tersedia');
    expect(formatNullableMetric(undefined, '%')).toBe('Belum tersedia');
    expect(formatNullableMetric(42.5, '%')).toBe('42.5%');
  });
});
