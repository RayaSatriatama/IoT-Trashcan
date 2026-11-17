import { useEffect, useRef, useState } from 'react';
import type { FirmwareMonitoringSample } from '../api/types';
import { MonitoringAgeTracker } from './liveSensorState';

/** One local clock for presentation and web-model validity, never a network poller. */
export function useLiveMonitoring(deviceId: string, monitoring: readonly FirmwareMonitoringSample[]): FirmwareMonitoringSample[] {
  const state = useRef<{ deviceId: string; tracker: MonitoringAgeTracker } | null>(null);
  const [, tick] = useState(0);
  if (!state.current || state.current.deviceId !== deviceId) {
    state.current = { deviceId, tracker: new MonitoringAgeTracker() };
  }
  const enabled = Boolean(deviceId && monitoring.length);
  useEffect(() => {
    if (!enabled) return;
    const timer = setInterval(() => tick(value => value + 1), 1000);
    return () => clearInterval(timer);
  }, [enabled]);
  return state.current.tracker.observe(monitoring, performance.now());
}
