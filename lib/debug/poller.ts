import type { DeviceBleSession } from '../ble/device-session';
import { createDebugApi, type DebugHistoryEvent, type DebugHistoryPage } from '../api/debug';
import type { DebugLevel, DebugMeta, DebugPage } from './types';

export type DebugSource = 'ble' | 'https';
export type DebugViewEvent = {
  source: DebugSource; event: DebugHistoryEvent | DebugPage['event'];
  bootId: string; buildId: string | null; receivedAt?: string; cursor?: string;
  snapshotLevel?: DebugLevel; capturedMonotonicUs?: string;
};
export type DebugViewSnapshot = Readonly<{
  visible: boolean; source: DebugSource; busy: boolean; error: string | null;
  events: readonly DebugViewEvent[]; meta: DebugMeta | null; history: DebugHistoryPage | null;
  level: DebugLevel | null; after: string; operatorAuthorized: boolean;
  firmwareGap: boolean; bootChanged: boolean;
}>;
type Options = {
  deviceId: string; session: DeviceBleSession;
  history?: (deviceId: string, after: string, limit: number, signal?: AbortSignal) => Promise<DebugHistoryPage>;
  intervalMs?: number; setTimer?: typeof setTimeout; clearTimer?: typeof clearTimeout;
};

export class DebugViewController {
  private deviceId: string;
  private source: DebugSource = 'ble';
  private visible = false;
  private disposed = false;
  private generation = 0;
  private pending = false;
  private timer: ReturnType<typeof setTimeout> | undefined;
  private abort: AbortController | undefined;
  private events: DebugViewEvent[] = [];
  private meta: DebugMeta | null = null;
  private history: DebugHistoryPage | null = null;
  private level: DebugLevel | null = null;
  private after = '0';
  private bearerToken: string | null = null;
  private error: string | null = null;
  private firmwareGap = false;
  private bootChanged = false;
  private listeners = new Set<() => void>();
  private snapshot: DebugViewSnapshot;
  private readonly intervalMs: number;
  private readonly setTimer: typeof setTimeout;
  private readonly clearTimer: typeof clearTimeout;
  private readonly unsubscribeSession: () => void;
  private peerWasEligible: boolean;

  constructor(private readonly options: Options) {
    this.deviceId = options.deviceId;
    this.intervalMs = Math.max(5000, options.intervalMs ?? 5000);
    this.setTimer = options.setTimer ?? globalThis.setTimeout.bind(globalThis);
    this.clearTimer = options.clearTimer ?? globalThis.clearTimeout.bind(globalThis);
    this.snapshot = this.makeSnapshot();
    this.peerWasEligible = this.peerEligible();
    this.unsubscribeSession = options.session.subscribe(() => {
      if (this.disposed) return;
      const eligible = this.peerEligible(), previous = this.peerWasEligible;
      this.peerWasEligible = eligible;
      if (!eligible) {
        if (previous) this.invalidate();
      } else if (!previous && this.visible && !this.pending) {
        void this.refresh();
      }
    });
  }

  getSnapshot = (): DebugViewSnapshot => this.snapshot;
  subscribe = (listener: () => void): (() => void) => {
    if (this.disposed) return () => {};
    this.listeners.add(listener);
    return () => this.listeners.delete(listener);
  };

  setVisible(visible: boolean): void {
    if (this.disposed || this.visible === visible) return;
    this.visible = visible;
    this.invalidate(); this.publish();
    if (visible) void this.refresh();
  }
  selectSource(source: DebugSource): void {
    if (this.disposed || source === this.source) return;
    this.source = source;
    this.resetView(); this.invalidate();
    this.peerWasEligible = this.peerEligible(); this.publish();
    if (this.visible) void this.refresh();
  }
  authorizeOperator(token: string): void {
    if (this.disposed) return;
    this.invalidate(); this.resetView();
    this.bearerToken = token.trim() || null;
    this.publish();
    if (this.source === 'https' && this.visible) void this.refresh();
  }
  clearOperatorAuthorization(): void {
    if (this.disposed) return;
    this.bearerToken = null;
    if (this.source === 'https') { this.invalidate(); this.resetView(); }
    this.publish();
  }
  setDevice(deviceId: string): void {
    if (this.disposed || deviceId === this.deviceId) return;
    this.deviceId = deviceId;
    this.visible = false;
    this.bearerToken = null;
    this.invalidate(); this.resetView(); this.publish();
  }

  async refresh(): Promise<void> {
    if (this.disposed || !this.visible || this.pending || !this.peerEligible() ||
        this.source === 'https' && !this.bearerToken) return;
    const generation = this.generation, source = this.source;
    const controller = this.begin();
    try {
      if (source === 'ble') {
        const result = await this.options.session.run('debug', async client => {
          const meta = await client.debugMeta();
          if (!this.current(generation, source, controller)) return null;
          const cursor = this.meta?.boot_id === meta.boot_id ? this.after : '0';
          const page = await client.debugPage({ boot_id: meta.boot_id, after: cursor });
          return { meta, page };
        });
        if (result && this.current(generation, source, controller)) {
          if (result.page.boot_changed || result.page.boot_id !== result.meta.boot_id) {
            this.resetView(); this.bootChanged = true;
          } else {
            const changed = this.meta !== null && this.meta.boot_id !== result.meta.boot_id;
            if (changed) this.events = [];
            this.bootChanged = changed;
            this.firmwareGap = result.page.gap;
            this.meta = result.meta; this.level = result.meta.level;
            if (result.page.event) this.append({ source, event: result.page.event,
              bootId: result.meta.boot_id, buildId: result.meta.build_id,
              snapshotLevel: result.meta.level, capturedMonotonicUs: result.meta.captured_monotonic_us });
            this.after = result.page.next_after;
          }
        }
      } else {
        const token = this.bearerToken!;
        const history = this.options.history ?? ((deviceId, after, limit, signal) =>
          createDebugApi({ bearerToken: token }).history(deviceId, after, limit, signal));
        const page = await history(this.deviceId, this.after, 100, controller.signal);
        if (this.current(generation, source, controller)) {
          this.history = page;
          for (const event of page.events) this.append({ source, event,
            bootId: event.boot_id, buildId: event.build_id, receivedAt: event.received_at,
            cursor: event.ingestion_id, snapshotLevel: event.snapshot_level,
            capturedMonotonicUs: event.captured_monotonic_us });
          this.after = page.next_after;
        }
      }
    } catch (error) {
      if (this.current(generation, source, controller))
        this.error = error instanceof Error ? error.message : 'Debug refresh failed';
    } finally { this.finish(controller, generation, source); }
  }

  async setLocalLevel(level: DebugLevel): Promise<void> {
    if (this.disposed || this.source !== 'ble' || this.pending || !this.visible || !this.peerEligible()) return;
    const generation = this.generation, controller = this.begin();
    try {
      const response = await this.options.session.run('debug', client => client.setDebugLevel(level));
      if (this.current(generation, 'ble', controller)) {
        this.level = response;
        if (this.meta) this.meta = { ...this.meta, level: response };
      }
    } catch (error) {
      if (this.current(generation, 'ble', controller))
        this.error = error instanceof Error ? error.message : 'Level update failed';
    } finally { this.finish(controller, generation, 'ble'); }
  }
  clearView(): void { if (!this.disposed) { this.events = []; this.publish(); } }
  disconnect(): void {
    if (this.disposed) return;
    this.visible = false;
    this.invalidate(); this.resetView();
    this.options.session.close(); this.publish();
  }
  dispose(): void {
    if (this.disposed) return;
    this.disposed = true; this.visible = false; this.bearerToken = null;
    this.unsubscribeSession(); this.invalidate(); this.resetView();
    this.listeners.clear(); this.snapshot = this.makeSnapshot();
  }

  private peerEligible(): boolean {
    const peer = this.options.session.getSnapshot();
    return !peer.controlActive && (peer.busy === null || peer.busy === 'debug') &&
      (this.source !== 'ble' || peer.connected);
  }
  private begin(): AbortController {
    this.cancelTimer();
    const controller = new AbortController();
    this.pending = true; this.abort = controller; this.error = null;
    this.publish(); return controller;
  }
  private finish(controller: AbortController, generation: number, source: DebugSource): void {
    if (this.abort !== controller) return;
    this.pending = false; this.abort = undefined; this.publish();
    if (this.disposed || !this.visible || !this.peerEligible()) return;
    if (generation !== this.generation || source !== this.source) { void this.refresh(); return; }
    this.timer = this.setTimer(() => { this.timer = undefined; void this.refresh(); }, this.intervalMs);
  }
  private append(event: DebugViewEvent): void {
    if (!event.event || this.events.some(item => item.source === event.source &&
        item.bootId === event.bootId && item.event?.sequence === event.event?.sequence)) return;
    this.events.push(event);
    if (this.events.length > 200) this.events.splice(0, this.events.length - 200);
  }
  private current(generation: number, source: DebugSource, controller: AbortController): boolean {
    return !this.disposed && this.visible && generation === this.generation &&
      source === this.source && !controller.signal.aborted;
  }
  private cancelTimer(): void {
    if (this.timer !== undefined) this.clearTimer(this.timer);
    this.timer = undefined;
  }
  private invalidate(): void {
    this.generation++; this.cancelTimer(); this.abort?.abort();
  }
  private resetView(): void {
    this.events = []; this.meta = null; this.history = null;
    this.level = null; this.after = '0'; this.error = null;
    this.firmwareGap = false; this.bootChanged = false;
  }
  private makeSnapshot(): DebugViewSnapshot {
    return Object.freeze({ visible: this.visible, source: this.source, busy: this.pending,
      error: this.error, events: Object.freeze(this.events.slice()), meta: this.meta,
      history: this.history, level: this.level, after: this.after,
      operatorAuthorized: this.bearerToken !== null,
      firmwareGap: this.firmwareGap, bootChanged: this.bootChanged });
  }
  private publish(): void { this.snapshot = this.makeSnapshot(); for (const listener of this.listeners) listener(); }
}
