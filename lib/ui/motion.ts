export const DATA_MOTION_DURATION_MS = 400;

function clampProgress(value: number): number {
  return Math.min(1, Math.max(0, value));
}

export function easeOutCubic(progress: number): number {
  const bounded = clampProgress(progress);
  return 1 - Math.pow(1 - bounded, 3);
}

export function motionValueAtElapsed(
  from: number,
  to: number,
  elapsedMs: number,
  durationMs = DATA_MOTION_DURATION_MS,
): number {
  if (durationMs <= 0 || elapsedMs >= durationMs) return to;
  if (elapsedMs <= 0) return from;
  const progress = easeOutCubic(elapsedMs / durationMs);
  return from + (to - from) * progress;
}

export function roundMotionValue(value: number, precision: number): number {
  const safePrecision = Math.min(3, Math.max(0, Math.trunc(precision)));
  const factor = 10 ** safePrecision;
  return Math.round((value + Number.EPSILON) * factor) / factor;
}

export function shouldAnimateMotionValue(
  from: number | null,
  to: number | null,
  reduceMotion: boolean,
): boolean {
  if (reduceMotion || to === null || !Number.isFinite(to)) return false;
  return from === null || from !== to;
}
