/**
 * Web-side NH3 estimate for the MQ-135.
 *
 * Chain: firmware calibrated ADC pin mV -> AO -> q = Vc/AO - 1 -> q/q0 -> Winsen
 * digitized NH3 curve (log-log).
 *
 * The installed firmware ADC profile declares a 1:1 divider, so AO is the pin
 * voltage. A 10k/14.7k divider was tried earlier but implies Rs ~ 0.5 kOhm at the
 * current baseline, which is not clean air, so it was rejected.
 *
 * A number is always produced while a sample exists: inside the digitized domain
 * it is interpolated between the 13 measured points, outside it continues on the
 * fitted power law and is reported as an extrapolation. Only a missing sample
 * yields no value.
 */

/** ADC profile divider declared by the firmware (1:1). */
export const MQ135_DIVIDER = 1;
export const MQ135_CIRCUIT_MV = 5000;
/** Clean-air baseline in pin mV, declared by the operator. Same as AO at 1:1. */
export const MQ135_BASELINE_PIN_MV = 2700;

/** Winsen MQ135 v1.6 fig.3 digitized NH3 curve: concentration_ppm, Rs/R0. */
const CURVE: readonly (readonly [number, number])[] = [
  [10, 0.6559], [15, 0.5814], [20, 0.518], [30, 0.4395], [50, 0.3576],
  [70, 0.3114], [100, 0.2668], [150, 0.2202], [200, 0.1872], [300, 0.1542],
  [500, 0.121], [700, 0.1049], [1000, 0.0893],
];

const qOf = (pinMv: number) => MQ135_CIRCUIT_MV / (pinMv * MQ135_DIVIDER) - 1;
const Q0 = qOf(MQ135_BASELINE_PIN_MV);

/** Least-squares log-log fit of the digitized curve, used past both endpoints. */
const POWER_FIT = (() => {
  const points = CURVE.map(([ppm, ratio]) => [Math.log(ppm), Math.log(ratio)] as const);
  const n = points.length;
  const sumX = points.reduce((total, [x]) => total + x, 0);
  const sumY = points.reduce((total, [, y]) => total + y, 0);
  const sumXX = points.reduce((total, [x]) => total + x * x, 0);
  const sumXY = points.reduce((total, [x, y]) => total + x * y, 0);
  const slope = (n * sumXY - sumX * sumY) / (n * sumXX - sumX * sumX);
  return { slope, intercept: (sumY - slope * sumX) / n };
})();

export type Nh3Status = 'IN_RANGE' | 'EXTRAPOLATED' | 'UNAVAILABLE';

export interface Nh3Estimate {
  ppm: number | null;
  status: Nh3Status;
  ratio: number | null;
  aoMv: number | null;
  /** True when the ratio sits above the 10 ppm end of the digitized curve. */
  belowCurve: boolean;
}

function ppmFromRatio(ratio: number): { ppm: number; extrapolated: boolean } {
  const top = CURVE[0][1];
  const bottom = CURVE[CURVE.length - 1][1];
  if (ratio <= top && ratio >= bottom) {
    for (let i = 0; i < CURVE.length - 1; i += 1) {
      const [p1, r1] = CURVE[i];
      const [p2, r2] = CURVE[i + 1];
      if (ratio <= r1 && ratio >= r2) {
        const t = (Math.log(ratio) - Math.log(r1)) / (Math.log(r2) - Math.log(r1));
        return { ppm: Math.exp(Math.log(p1) + t * (Math.log(p2) - Math.log(p1))), extrapolated: false };
      }
    }
  }
  const ppm = Math.exp((Math.log(Math.max(ratio, 1e-6)) - POWER_FIT.intercept) / POWER_FIT.slope);
  return { ppm: Math.min(100000, Math.max(0.05, ppm)), extrapolated: true };
}

export function estimateNh3FromPinMv(pinMv: number | null | undefined): Nh3Estimate {
  if (typeof pinMv !== 'number' || !Number.isFinite(pinMv) || pinMv <= 0) {
    return { ppm: null, status: 'UNAVAILABLE', ratio: null, aoMv: null, belowCurve: false };
  }
  const aoMv = pinMv * MQ135_DIVIDER;
  const ratio = qOf(pinMv) / Q0;
  const { ppm, extrapolated } = ppmFromRatio(ratio);
  return {
    ppm,
    status: extrapolated ? 'EXTRAPOLATED' : 'IN_RANGE',
    ratio,
    aoMv,
    belowCurve: ratio > CURVE[0][1],
  };
}

/** Always a number while a sample exists; decimals shrink as the value grows. */
export function formatNh3Estimate(estimate: Nh3Estimate): string {
  if (estimate.ppm === null) return '—';
  const { ppm } = estimate;
  const text = ppm < 100 ? ppm.toFixed(1) : String(Math.round(ppm));
  return `${text.replace('.', ',')} ppm`;
}
