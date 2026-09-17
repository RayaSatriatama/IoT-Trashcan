import { useCallback, useEffect, useRef, useState } from 'react';

export interface PollingResource<T> {
  data: T | null;
  loading: boolean;
  error: Error | null;
  stale: boolean;
  retry: () => void;
}

function isAbortError(error: unknown): boolean {
  return error instanceof Error && error.name === 'AbortError';
}

export function usePollingResource<T>(
  loader: (signal: AbortSignal) => Promise<T>,
  intervalMs = 20_000,
): PollingResource<T> {
  const loaderRef = useRef(loader);
  const inFlightRef = useRef<AbortController | null>(null);
  const mountedRef = useRef(true);
  const dataRef = useRef<T | null>(null);
  const [data, setData] = useState<T | null>(null);
  const [loading, setLoading] = useState(true);
  const [error, setError] = useState<Error | null>(null);
  const [stale, setStale] = useState(false);

  loaderRef.current = loader;

  const retry = useCallback(() => {
    if (!mountedRef.current || inFlightRef.current) return;
    const controller = new AbortController();
    inFlightRef.current = controller;
    setLoading(dataRef.current === null);
    loaderRef.current(controller.signal)
      .then((next) => {
        if (!mountedRef.current || controller.signal.aborted) return;
        dataRef.current = next;
        setData(next);
        setError(null);
        setStale(false);
      })
      .catch((reason: unknown) => {
        if (!mountedRef.current || controller.signal.aborted || isAbortError(reason)) return;
        const nextError = reason instanceof Error ? reason : new Error('Request failed');
        setError(nextError);
        setStale(dataRef.current !== null);
      })
      .finally(() => {
        if (inFlightRef.current === controller) {
          inFlightRef.current = null;
          if (mountedRef.current) setLoading(false);
        }
      });
  }, []);

  useEffect(() => {
    mountedRef.current = true;
    retry();
    const timer = setInterval(retry, intervalMs);
    return () => {
      mountedRef.current = false;
      clearInterval(timer);
      inFlightRef.current?.abort();
      inFlightRef.current = null;
    };
  }, [intervalMs, retry]);

  return { data, loading, error, stale, retry };
}
