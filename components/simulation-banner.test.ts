import { describe, expect, it } from 'vitest';

import { simulationBannerCopy } from './SimulationBanner';

describe('simulation banner', () => {
  it('is absent when runtime simulation is disabled', () => {
    expect(simulationBannerCopy(false)).toBeNull();
  });

  it('clearly labels synthetic telemetry when enabled', () => {
    expect(simulationBannerCopy(true)).toEqual({
      title: 'MODE SIMULASI',
      description: 'Data sensor pada halaman ini dibuat secara sintetis.',
    });
  });
});
