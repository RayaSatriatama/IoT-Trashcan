import { describe, expect, it } from 'vitest';

import { simulationIndicatorVisible } from './SimulationIndicator';

describe('simulation indicator visibility', () => {
  it('hides the indicator when the runtime disables it', () => {
    expect(simulationIndicatorVisible({ simulation: true, simulationIndicator: false })).toBe(false);
  });

  it('hides the indicator when simulation is inactive', () => {
    expect(simulationIndicatorVisible({ simulation: false, simulationIndicator: true })).toBe(false);
  });

  it('shows the indicator only for an enabled simulation runtime', () => {
    expect(simulationIndicatorVisible({ simulation: true, simulationIndicator: true })).toBe(true);
  });
});
