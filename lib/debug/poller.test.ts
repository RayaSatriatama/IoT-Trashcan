import { afterEach, describe, expect, it, vi } from 'vitest';
import { DebugViewController } from './poller';
import { DeviceBleSession } from '../ble/device-session';
import type { DebugMeta, DebugPage } from './types';
const meta: DebugMeta = { boot_id: '0123456789abcdef0123456789abcdef', build_id: 'test-build', level: 'INFO', oldest_sequence: '1', latest_sequence: '9007199254740993', overwritten: '0', delivery_lost: '0', usb_lost: '0', captured_monotonic_us: '99', sequence_exhausted: false };
const page: DebugPage = { boot_id: meta.boot_id, level: 'INFO', oldest_sequence: '1', next_after: '9007199254740993', overwritten: '0', gap: false, boot_changed: false, event: { sequence: '9007199254740993', monotonic_us: '98', level: 'INFO', module: 1, event_code: 1, request_id: null, duration_us: 4, arg0: 0, arg1: 0, related_sequence: null } };
function fixture() {
  const metaCall = vi.fn(async () => meta), pageCall = vi.fn(async () => page), levelCall = vi.fn(async (level: any) => level);
  const client: any = { connect: vi.fn(async () => {}), close: vi.fn(), verifyDeviceIdentity: vi.fn(async (id: string) => id), debugMeta: metaCall, debugPage: pageCall, setDebugLevel: levelCall };
  const session = new DeviceBleSession('dev', { createClient: () => client });
  const history = vi.fn(async () => ({ device_id: 'dev', events: [], next_after: '0', oldest_available: '0', retention_gap: false, has_more: false })); const timers: Array<() => void> = [];
  const controller = new DebugViewController({ deviceId: 'dev', session, history, setTimer: ((fn: any) => { timers.push(fn); return timers.length as any; }) as any, clearTimer: vi.fn() as any });
  return { controller, session, history, client, timers, connect: async () => { await session.connect(); } };
}
afterEach(() => { vi.restoreAllMocks(); vi.unstubAllGlobals(); });
describe('DebugViewController', () => {
  it('invokes default browser timers with their global receiver when polling and hiding', async () => {
    const callbacks: Array<() => void> = [];
    const delays: number[] = [];
    const cancelled: unknown[] = [];
    vi.stubGlobal('setTimeout', function (this: unknown, callback: () => void, delay: number) {
      if (this !== globalThis) throw new TypeError('Illegal invocation');
      callbacks.push(callback); delays.push(delay); return callbacks.length;
    });
    vi.stubGlobal('clearTimeout', function (this: unknown, handle: unknown) {
      if (this !== globalThis) throw new TypeError('Illegal invocation');
      cancelled.push(handle);
    });
    let eligible = false;
    const client = { debugMeta: async () => meta, debugPage: async () => page };
    const session = {
      getSnapshot: () => ({ connected: eligible, controlActive: false, busy: null }),
      subscribe: () => () => {},
      run: async (_owner: string, operation: (value: typeof client) => unknown) => operation(client),
    } as unknown as DeviceBleSession;
    const controller = new DebugViewController({ deviceId: 'fixture', session });
    controller.setVisible(true); eligible = true;
    try {
      await expect(controller.refresh()).resolves.toBeUndefined();
      expect(delays).toEqual([5000]);
      expect(callbacks).toHaveLength(1);
      expect(() => controller.setVisible(false)).not.toThrow();
      expect(cancelled).toEqual([1]);
    } finally { controller.dispose(); }
  });
  it.each(['resolve', 'reject'])('source switch waits for old %s then admits exactly one current-generation BLE read', async outcome => {
    const f = fixture(); await f.connect();
    let resolve!: (value: any) => void, reject!: (error: Error) => void;
    f.history.mockReturnValueOnce(new Promise((yes, no) => { resolve = yes; reject = no; }) as any);
    f.controller.selectSource('https'); f.controller.authorizeOperator('TEST-old'); f.controller.setVisible(true);
    f.controller.selectSource('ble'); await f.controller.refresh();
    expect(f.client.debugMeta).not.toHaveBeenCalled();
    if (outcome === 'resolve') resolve({ device_id: 'dev', events: [], next_after: '0', oldest_available: '0', retention_gap: false, has_more: false });
    else reject(new Error('stale TEST failure'));
    await vi.waitFor(() => expect(f.controller.getSnapshot().events).toHaveLength(1));
    expect(f.client.debugMeta).toHaveBeenCalledTimes(1);
    expect(f.controller.getSnapshot()).toMatchObject({ source: 'ble', history: null, error: null });
    expect(f.timers).toHaveLength(1);
    f.controller.dispose(); f.session.dispose();
  });
  it.each(['resolve', 'reject'])('device replacement clears authorization and ignores late %s without new requests', async outcome => {
    const f = fixture();
    let resolve!: (value: any) => void, reject!: (error: Error) => void;
    f.history.mockReturnValueOnce(new Promise((yes, no) => { resolve = yes; reject = no; }) as any);
    f.controller.selectSource('https'); f.controller.authorizeOperator('TEST-old'); f.controller.setVisible(true);
    f.controller.setDevice('other');
    if (outcome === 'resolve') resolve({ device_id: 'dev', events: [], next_after: '0', oldest_available: '0', retention_gap: false, has_more: false });
    else reject(new Error('stale TEST failure'));
    await vi.waitFor(() => expect(f.controller.getSnapshot().busy).toBe(false));
    expect(f.history).toHaveBeenCalledTimes(1);
    expect(f.timers).toHaveLength(0);
    expect(f.controller.getSnapshot()).toMatchObject({ visible: false, events: [], error: null, operatorAuthorized: false });
    f.controller.dispose(); f.session.dispose();
  });
  it('keeps a pending level mutation single-send and rejects its late acknowledgement after a source switch', async () => {
    const f = fixture(); await f.connect(); f.controller.setVisible(true);
    await vi.waitFor(() => expect(f.controller.getSnapshot().events).toHaveLength(1));
    let resolve!: (value: any) => void;
    f.client.setDebugLevel.mockReturnValueOnce(new Promise(yes => { resolve = yes; }));
    const update = f.controller.setLocalLevel('DEBUG');
    await f.controller.setLocalLevel('ERROR');
    expect(f.client.setDebugLevel).toHaveBeenCalledTimes(1);
    f.controller.selectSource('https');
    resolve('DEBUG'); await update;
    expect(f.controller.getSnapshot()).toMatchObject({ level: null, meta: null, source: 'https', busy: false });
    f.controller.dispose(); f.session.dispose();
  });
  it('reports a BLE gap and resynchronizes an explicit boot change without displaying its unverified page', async () => {
    const f = fixture(); await f.connect();
    f.client.debugPage.mockResolvedValueOnce({ ...page, gap: true });
    f.controller.setVisible(true);
    await vi.waitFor(() => expect(f.controller.getSnapshot().busy).toBe(false));
    expect(f.controller.getSnapshot()).toMatchObject({ firmwareGap: true });
    const boot = 'ffffffffffffffffffffffffffffffff';
    f.client.debugPage.mockResolvedValueOnce({ ...page, boot_id: boot, boot_changed: true, event: null });
    await f.controller.refresh();
    expect(f.controller.getSnapshot()).toMatchObject({ bootChanged: true, meta: null, events: [], after: '0' });
    f.client.debugMeta.mockResolvedValueOnce({ ...meta, boot_id: boot });
    f.client.debugPage.mockResolvedValueOnce({ ...page, boot_id: boot });
    await f.controller.refresh();
    expect(f.client.debugPage).toHaveBeenLastCalledWith({ boot_id: boot, after: '0' });
    expect(f.controller.getSnapshot().events[0].bootId).toBe(boot);
    f.controller.dispose(); f.session.dispose();
  });
  it('automatically pauses for a real calibration lease without an error and resumes on release', async () => {
    const f = fixture(); await f.connect();
    const release = f.session.beginControlActivity('https-calibration');
    f.controller.setVisible(true);
    await Promise.resolve(); await Promise.resolve();
    expect(f.controller.getSnapshot().error).toBeNull();
    expect(f.client.debugMeta).not.toHaveBeenCalled();
    release();
    await vi.waitFor(() => expect(f.client.debugPage).toHaveBeenCalledTimes(1));
    expect(f.controller.getSnapshot().error).toBeNull();
    f.controller.dispose(); f.session.dispose();
  });
  it('requires explicit HTTPS authorization and keeps cursor as exact text', async () => {
    const f = fixture(); f.controller.selectSource('https'); f.controller.setVisible(true); await Promise.resolve(); expect(f.history).not.toHaveBeenCalled();
    f.controller.authorizeOperator('operator-token'); await f.controller.refresh(); expect(f.history).toHaveBeenCalledWith('dev', '0', 100, expect.any(AbortSignal)); expect(f.controller.getSnapshot().operatorAuthorized).toBe(true);
    f.controller.dispose(); expect(f.controller.getSnapshot().operatorAuthorized).toBe(false);
  });
  it('clears authorization and old-device content when the source device changes', async () => {
    const f = fixture(); f.controller.selectSource('https'); f.controller.authorizeOperator('token'); f.controller.setDevice('other');
    expect(f.controller.getSnapshot()).toMatchObject({ events: [], history: null, operatorAuthorized: false, after: '0' });
    f.controller.setVisible(true); await f.controller.refresh(); expect(f.history).not.toHaveBeenCalled(); f.controller.dispose();
  });
  it('reads BLE META then PAGE through shared owner and retains large decimal IDs', async () => {
    const f = fixture(); await f.connect(); f.controller.setVisible(true); await vi.waitFor(() => expect(f.client.debugPage).toHaveBeenCalled());
    expect(f.client.debugPage).toHaveBeenCalledWith({ boot_id: meta.boot_id, after: '0' }); expect(f.controller.getSnapshot().after).toBe('9007199254740993'); expect(f.controller.getSnapshot().events[0].event?.sequence).toBe('9007199254740993'); f.controller.dispose(); f.session.dispose();
  });
  it('aborts late HTTP responses when hidden and does not schedule another poll', async () => {
    const f = fixture(); let finish!: (value: any) => void; f.history.mockReturnValueOnce(new Promise(resolve => { finish = resolve; }) as any);
    f.controller.selectSource('https'); f.controller.authorizeOperator('token'); f.controller.setVisible(true); await Promise.resolve(); f.controller.setVisible(false);
    finish({ device_id: 'dev', events: [], next_after: '0', oldest_available: '0', retention_gap: false, has_more: false }); await Promise.resolve(); expect(f.controller.getSnapshot().history).toBeNull(); expect(f.timers).toHaveLength(0); f.controller.dispose();
  });
  it('does not admit another request until an aborted generation actually settles', async () => {
    const f = fixture(); let finish!: (value: any) => void; f.history.mockReturnValueOnce(new Promise(resolve => { finish = resolve; }) as any);
    f.controller.selectSource('https'); f.controller.authorizeOperator('token'); f.controller.setVisible(true); await Promise.resolve(); f.controller.setVisible(false); f.controller.setVisible(true); await f.controller.refresh();
    expect(f.history).toHaveBeenCalledTimes(1); finish({ device_id: 'dev', events: [], next_after: '0', oldest_available: '0', retention_gap: false, has_more: false }); await vi.waitFor(() => expect(f.controller.getSnapshot().busy).toBe(false)); f.controller.dispose();
  });
  it('uses shared busy admission to pause during calibration and disconnect clears view', async () => {
    const f = fixture(); await f.connect(); const release = f.session.beginControlActivity('ble-calibration'); f.controller.setVisible(true); await Promise.resolve(); expect(f.client.debugMeta).not.toHaveBeenCalled();
    release(); await f.controller.refresh(); expect(f.client.debugMeta).toHaveBeenCalledTimes(1); f.controller.disconnect(); expect(f.controller.getSnapshot().events).toHaveLength(0); f.controller.dispose(); f.session.dispose();
  });
  it('clears the local event list explicitly', async () => {
    const f = fixture(); await f.connect(); f.controller.setVisible(true); await vi.waitFor(() => expect(f.controller.getSnapshot().events.length).toBe(1)); f.controller.clearView(); expect(f.controller.getSnapshot().events).toHaveLength(0); f.controller.dispose(); f.session.dispose();
  });
  it('caps retained view rows at 200 and keeps source, boot and sequence identity separate', async () => {
    const f = fixture(); f.controller.selectSource('https'); f.controller.authorizeOperator('token');
    const events = Array.from({ length: 201 }, (_, i) => ({ sequence: String(i + 1), monotonic_us: String(i), level: 'INFO' as const, module: 1, event_code: 1, request_id: null, duration_us: 0, arg0: 0, arg1: 0, related_sequence: null, ingestion_id: String(1000 + i), received_at: '2026-10-26T00:00:00Z', boot_id: meta.boot_id, build_id: 'test', snapshot_level: 'INFO' as const, captured_monotonic_us: '500', oldest_sequence: '1', overwritten: '0', delivery_lost: '0' }));
    f.history.mockImplementationOnce(async () => ({ device_id: 'dev', events: events as unknown as never[], next_after: '1200', oldest_available: '1000', retention_gap: true, has_more: false }));
    f.controller.setVisible(true); await vi.waitFor(() => expect(f.controller.getSnapshot().events.length).toBe(200));
    expect(f.controller.getSnapshot().events[0].event?.sequence).toBe('2'); expect(f.controller.getSnapshot().events[0].cursor).toBe('1001'); expect(f.controller.getSnapshot().history?.retention_gap).toBe(true); f.controller.dispose();
  });
});
