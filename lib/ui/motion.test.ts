import { describe, expect, it } from 'vitest';

import {
  easeOutCubic,
  motionValueAtElapsed,
  roundMotionValue,
  shouldAnimateMotionValue,
} from './motion';

describe('monitoring motion helpers', () => {
  it('keeps number interpolation bounded and lands exactly on the target', () => {
    expect(motionValueAtElapsed(10, 30, -20, 400)).toBe(10);
    expect(motionValueAtElapsed(10, 30, 200, 400)).toBeCloseTo(27.5);
    expect(motionValueAtElapsed(10, 30, 400, 400)).toBe(30);
    expect(motionValueAtElapsed(10, 30, 900, 400)).toBe(30);
  });

  it('uses a calm ease-out curve without overshooting', () => {
    expect(easeOutCubic(0)).toBe(0);
    expect(easeOutCubic(0.5)).toBeCloseTo(0.875);
    expect(easeOutCubic(1)).toBe(1);
  });

  it('rounds animated values to the presentation precision', () => {
    expect(roundMotionValue(18.76, 0)).toBe(19);
    expect(roundMotionValue(18.76, 1)).toBe(18.8);
    expect(roundMotionValue(18.765, 2)).toBe(18.77);
  });

  it('animates only finite changed values when motion is allowed', () => {
    expect(shouldAnimateMotionValue(10, 20, false)).toBe(true);
    expect(shouldAnimateMotionValue(10, 10, false)).toBe(false);
    expect(shouldAnimateMotionValue(10, 20, true)).toBe(false);
    expect(shouldAnimateMotionValue(null, 20, false)).toBe(true);
    expect(shouldAnimateMotionValue(10, Number.NaN, false)).toBe(false);
  });

  it('does not restart a data transition when the target is unchanged', () => {
    expect(shouldAnimateMotionValue(0, 0, false)).toBe(false);
    expect(shouldAnimateMotionValue(42.5, 42.5, false)).toBe(false);
  });

  it('settles data transitions immediately when reduced motion is enabled', () => {
    expect(shouldAnimateMotionValue(0, 42, true)).toBe(false);
    expect(shouldAnimateMotionValue(42, 0, true)).toBe(false);
  });
});
