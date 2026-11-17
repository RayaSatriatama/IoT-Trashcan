import { useCallback, useEffect, useState } from 'react';

/**
 * Elapsed time since the first reading arrived, until now, resettable by the
 * operator. The start instant is kept in localStorage so a reload does not
 * silently restart the window; only an explicit reset does.
 *
 * This is a web-side observation window, not a firmware accumulation counter and
 * not proof that the slot was continuously occupied.
 */
const STORAGE_PREFIX = 'sigap.accumulation.since.';

function readStart(key: string): number | null {
  if (typeof window === 'undefined') return null;
  try {
    const raw = window.localStorage.getItem(key);
    if (!raw) return null;
    const parsed = Number.parseInt(raw, 10);
    return Number.isFinite(parsed) && parsed > 0 ? parsed : null;
  } catch {
    return null;
  }
}

function writeStart(key: string, value: number | null) {
  if (typeof window === 'undefined') return;
  try {
    if (value === null) window.localStorage.removeItem(key);
    else window.localStorage.setItem(key, String(value));
  } catch {
    /* storage can be unavailable; the timer then only lives in memory */
  }
}

export interface AccumulationTimer {
  /** Milliseconds since the first reading, or null before any reading. */
  elapsedMs: number | null;
  startIso: string | null;
  reset: () => void;
}

export function useAccumulationTimer(deviceId: string, active: boolean): AccumulationTimer {
  const key = `${STORAGE_PREFIX}${deviceId}`;
  const [start, setStart] = useState<number | null>(() => readStart(key));
  const [now, setNow] = useState(() => Date.now());

  // Start counting on the first reading, never on an empty device.
  useEffect(() => {
    if (!active || start !== null) return;
    const started = Date.now();
    writeStart(key, started);
    setStart(started);
    setNow(started);
  }, [active, key, start]);

  useEffect(() => {
    if (start === null) return;
    const timer = setInterval(() => setNow(Date.now()), 1000);
    return () => clearInterval(timer);
  }, [start]);

  const reset = useCallback(() => {
    const started = Date.now();
    writeStart(key, started);
    setStart(started);
    setNow(started);
  }, [key]);

  if (start === null) return { elapsedMs: null, startIso: null, reset };
  return {
    elapsedMs: Math.max(0, now - start),
    startIso: new Date(start).toISOString(),
    reset,
  };
}

/** "2 jam 14 mnt" / "14 mnt" / "45 dtk" — coarse on purpose, the tile is narrow. */
export function formatElapsed(elapsedMs: number | null): string {
  if (elapsedMs === null) return '—';
  const totalSeconds = Math.floor(elapsedMs / 1000);
  const days = Math.floor(totalSeconds / 86400);
  const hours = Math.floor((totalSeconds % 86400) / 3600);
  const minutes = Math.floor((totalSeconds % 3600) / 60);
  if (days > 0) return `${days} hr ${hours} jam`;
  if (hours > 0) return `${hours} jam ${minutes} mnt`;
  if (minutes > 0) return `${minutes} mnt`;
  return `${totalSeconds} dtk`;
}
