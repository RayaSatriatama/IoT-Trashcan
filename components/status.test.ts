import { describe, expect, it } from 'vitest';

import { getStatusBadgePresentation } from './StatusBadge';
import { formatMetricValue } from './MetricBar';

describe('shared monitoring presentation', () => {
  it('keeps canonical status labels and tokens together', () => {
    expect(getStatusBadgePresentation('Safety Alert')).toEqual({
      label: 'Peringatan keselamatan',
      token: 'statusSafetyAlert',
    });
  });

  it('renders a nullable metric as unavailable instead of zero', () => {
    expect(formatMetricValue(null, '%')).toBe('Belum tersedia');
    expect(formatMetricValue(0, '%')).toBe('0%');
  });
});
