import { useEffect, useRef, useState } from 'react';

import {
  DATA_MOTION_DURATION_MS,
  motionValueAtElapsed,
  roundMotionValue,
  shouldAnimateMotionValue,
} from './motion';
import { useReducedMotion } from './motionContext';

export function useAnimatedNumber(
  value: number | null | undefined,
  options: { durationMs?: number; precision?: number } = {},
): number | null {
  const reduceMotion = useReducedMotion();
  const durationMs = options.durationMs ?? DATA_MOTION_DURATION_MS;
  const precision = options.precision ?? 0;
  const target = typeof value === 'number' && Number.isFinite(value) ? value : null;
  const initialValue = target === null ? null : reduceMotion ? roundMotionValue(target, precision) : 0;
  const [displayValue, setDisplayValue] = useState<number | null>(initialValue);
  const displayRef = useRef<number | null>(initialValue);
  const frameRef = useRef<ReturnType<typeof requestAnimationFrame> | null>(null);

  useEffect(() => {
    if (frameRef.current !== null) cancelAnimationFrame(frameRef.current);
    frameRef.current = null;

    if (target === null) {
      displayRef.current = null;
      setDisplayValue(null);
      return undefined;
    }

    const from = displayRef.current ?? 0;
    if (!shouldAnimateMotionValue(displayRef.current, target, reduceMotion)) {
      const finalValue = roundMotionValue(target, precision);
      displayRef.current = finalValue;
      setDisplayValue(finalValue);
      return undefined;
    }

    let startedAt: number | null = null;
    const update = (timestamp: number) => {
      startedAt ??= timestamp;
      const elapsed = timestamp - startedAt;
      const nextValue = roundMotionValue(
        motionValueAtElapsed(from, target, elapsed, durationMs),
        precision,
      );
      if (displayRef.current !== nextValue) {
        displayRef.current = nextValue;
        setDisplayValue(nextValue);
      }
      if (elapsed < durationMs) {
        frameRef.current = requestAnimationFrame(update);
      } else {
        const finalValue = roundMotionValue(target, precision);
        displayRef.current = finalValue;
        setDisplayValue(finalValue);
        frameRef.current = null;
      }
    };

    frameRef.current = requestAnimationFrame(update);
    return () => {
      if (frameRef.current !== null) cancelAnimationFrame(frameRef.current);
      frameRef.current = null;
    };
  }, [durationMs, precision, reduceMotion, target]);

  return displayValue;
}
