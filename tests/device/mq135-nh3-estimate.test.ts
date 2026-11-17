import { describe, expect, it } from 'vitest';
import { MQ135_BASELINE_PIN_MV, MQ135_DIVIDER, estimateNh3FromPinMv, formatNh3Estimate } from '../../lib/ui/mq135Nh3Estimate';

/**
 * The firmware ADC profile declares a 1:1 divider, so AO is the pin voltage. The
 * estimate must always return a number while a sample exists; outside the
 * digitized 10-1000 ppm window it continues on the fitted power law.
 */
describe('MQ135 NH3 estimate at the declared 1:1 divider', () => {
  it('uses the firmware divider and the operator baseline', () => {
    expect(MQ135_DIVIDER).toBe(1);
    expect(MQ135_BASELINE_PIN_MV).toBe(2700);
  });

  it('always yields a number for any plausible pin voltage', () => {
    for (const pin of [2600, 2700, 2750, 2800, 2829, 2834, 2847, 2900, 2999]) {
      const got = estimateNh3FromPinMv(pin);
      expect(got.status, `pin ${pin}`).not.toBe('UNAVAILABLE');
      expect(got.ppm, `pin ${pin}`).not.toBeNull();
      expect(Number.isFinite(got.ppm as number), `pin ${pin}`).toBe(true);
      expect(formatNh3Estimate(got), `pin ${pin}`).toMatch(/ppm$/);
    }
  });

  it('sits in the single-digit ppm range for clean air at the current operating point', () => {
    // Baseline 2700 mV is 1.000 ratio and lands below the 10 ppm end of the
    // digitized curve, so these are extrapolations, not curve readings.
    expect(estimateNh3FromPinMv(2700).belowCurve).toBe(true);
    expect(estimateNh3FromPinMv(2700).status).toBe('EXTRAPOLATED');
    expect(estimateNh3FromPinMv(2700).ppm).toBeCloseTo(4.56, 1);
    expect(estimateNh3FromPinMv(2834).ppm).toBeCloseTo(5.82, 1);
    expect(estimateNh3FromPinMv(2847).ppm).toBeCloseTo(5.97, 1);
  });

  it('is monotone: higher pin voltage never lowers the estimate', () => {
    let previous = -Infinity;
    for (let pin = 2600; pin <= 2990; pin += 10) {
      const value = estimateNh3FromPinMv(pin).ppm as number;
      expect(value).toBeGreaterThanOrEqual(previous);
      previous = value;
    }
  });

  it('reports no value only when the sample is missing or invalid', () => {
    expect(estimateNh3FromPinMv(null).status).toBe('UNAVAILABLE');
    expect(estimateNh3FromPinMv(undefined).ppm).toBeNull();
    expect(estimateNh3FromPinMv(0).ppm).toBeNull();
    expect(formatNh3Estimate(estimateNh3FromPinMv(null))).toBe('—');
  });
});
