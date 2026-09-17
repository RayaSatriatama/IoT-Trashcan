import { afterEach, describe, expect, it, vi } from 'vitest';

import { PollingController } from './hooks';

afterEach(() => {
  vi.useRealTimers();
});

describe('PollingController', () => {
  it('polls every 20 seconds and aborts the previous request before replacement', async () => {
    vi.useFakeTimers();
    const signals: AbortSignal[] = [];
    const loader = vi.fn((signal: AbortSignal) => {
      signals.push(signal);
      return new Promise<string>(() => undefined);
    });
    const controller = new PollingController(loader, vi.fn(), vi.fn());

    controller.start();
    expect(loader).toHaveBeenCalledTimes(1);
    await vi.advanceTimersByTimeAsync(19_999);
    expect(loader).toHaveBeenCalledTimes(1);
    await vi.advanceTimersByTimeAsync(1);
    expect(loader).toHaveBeenCalledTimes(2);
    expect(signals[0].aborted).toBe(true);
    controller.stop();
  });

  it('replaces an active request when manually refreshed', async () => {
    const signals: AbortSignal[] = [];
    const loader = vi.fn((signal: AbortSignal) => {
      signals.push(signal);
      return new Promise<string>(() => undefined);
    });
    const controller = new PollingController(loader, vi.fn(), vi.fn());
    controller.start();
    void controller.refresh();
    expect(loader).toHaveBeenCalledTimes(2);
    expect(signals[0].aborted).toBe(true);
    controller.stop();
  });

  it('reports non-abort failures to the consumer', async () => {
    const error = new Error('offline');
    const onError = vi.fn();
    const controller = new PollingController(
      async () => { throw error; },
      vi.fn(),
      onError,
    );
    controller.start();
    await vi.waitFor(() => expect(onError).toHaveBeenCalledWith(error));
    controller.stop();
  });

  it('aborts an active request when stopped', () => {
    let signal: AbortSignal | undefined;
    const controller = new PollingController(
      async (nextSignal) => {
        signal = nextSignal;
        return new Promise<string>(() => undefined);
      },
      vi.fn(),
      vi.fn(),
    );
    controller.start();
    controller.stop();
    expect(signal?.aborted).toBe(true);
  });
});
