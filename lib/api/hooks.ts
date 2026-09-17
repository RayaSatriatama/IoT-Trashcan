import { useCallback, useEffect, useRef, useState } from 'react';

export interface PollingResource<T> {
  data: T | null;
  loading: boolean;
  error: Error | null;
  stale: boolean;
  retry: () => void;
}

export type PollingLoader<T> = (signal: AbortSignal) => Promise<T>;

function isAbortError(error: unknown): boolean {
  return error instanceof Error && error.name === 'AbortError';
}

/** Coordinates one cancellable request and one interval without overlap. */
export class PollingController<T> {
  private readonly loader: PollingLoader<T>;
  private readonly onSuccess: (value: T) => void;
  private readonly onError: (error: Error) => void;
  private readonly intervalMs: number;
  private active: { controller: AbortController } | null = null;
  private timer: ReturnType<typeof setInterval> | null = null;
  private running = false;

  constructor(
    loader: PollingLoader<T>,
    onSuccess: (value: T) => void,
    onError: (error: Error) => void,
    intervalMs = 20_000,
  ) {
    this.loader = loader;
    this.onSuccess = onSuccess;
    this.onError = onError;
    this.intervalMs = intervalMs;
  }

  start(): void {
    if (this.running) return;
    this.running = true;
    void this.refresh();
    this.timer = setInterval(() => void this.refresh(), this.intervalMs);
  }

  async refresh(): Promise<void> {
    if (!this.running) return;
    this.active?.controller.abort();
    const controller = new AbortController();
    this.active = { controller };
    try {
      const value = await this.loader(controller.signal);
      if (this.running && !controller.signal.aborted && this.active?.controller === controller) {
        this.onSuccess(value);
      }
    } catch (reason: unknown) {
      if (!this.running || controller.signal.aborted || isAbortError(reason) || this.active?.controller !== controller) return;
      this.onError(reason instanceof Error ? reason : new Error('Request failed'));
    } finally {
      if (this.active?.controller === controller) this.active = null;
    }
  }

  stop(): void {
    this.running = false;
    if (this.timer) clearInterval(this.timer);
    this.timer = null;
    this.active?.controller.abort();
    this.active = null;
  }
}

export function usePollingResource<T>(
  loader: PollingLoader<T>,
  intervalMs = 20_000,
): PollingResource<T> {
  const dataRef = useRef<T | null>(null);
  const controllerRef = useRef<PollingController<T> | null>(null);
  const [data, setData] = useState<T | null>(null);
  const [loading, setLoading] = useState(true);
  const [error, setError] = useState<Error | null>(null);
  const [stale, setStale] = useState(false);

  useEffect(() => {
    setLoading(dataRef.current === null);
    setError(null);
    setStale(false);
    const controller = new PollingController(
      loader,
      (value) => {
        dataRef.current = value;
        setData(value);
        setError(null);
        setStale(false);
        setLoading(false);
      },
      (reason) => {
        setError(reason);
        setStale(dataRef.current !== null);
        setLoading(false);
      },
      intervalMs,
    );
    controllerRef.current = controller;
    controller.start();
    return () => {
      controller.stop();
      if (controllerRef.current === controller) controllerRef.current = null;
    };
  }, [intervalMs, loader]);

  const retry = useCallback(() => {
    void controllerRef.current?.refresh();
  }, []);

  return { data, loading, error, stale, retry };
}
