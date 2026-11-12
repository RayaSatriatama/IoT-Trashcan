import { describe, expect, it, vi } from 'vitest';
import type { BleCalibrationResponse, DebugCursor, DebugLevel, DebugMeta, DebugPage, WifiNetwork, WifiStatus } from './protocol';
import type { BleClientDiagnosticEvent, BleProvisioningClientOptions } from './client';
import { DeviceBleSession, type PublicBleClient } from './device-session';

const ID = '0000000000000001';
const deferred = <T,>() => {
  let resolve!: (value: T) => void;
  let reject!: (reason?: unknown) => void;
  const promise = new Promise<T>((yes, no) => { resolve = yes; reject = no; });
  return { promise, resolve, reject };
};

function fakeClient(options: BleProvisioningClientOptions, overrides: Partial<PublicBleClient> = {}) {
  const client: PublicBleClient = {
    connect: vi.fn(async () => { options.onConnectionChange?.(true); options.onDeviceIdentityChange?.('SIGAP-test'); }),
    close: vi.fn(() => { options.onConnectionChange?.(false); options.onDeviceIdentityChange?.(null); options.onVerifiedDeviceIdentityChange?.(null); }),
    verifyDeviceIdentity: vi.fn(async (expected: string) => { options.onVerifiedDeviceIdentityChange?.(expected); return expected; }),
    get verifiedDeviceId() { return ID; },
    debugMeta: vi.fn(async () => ({} as DebugMeta)), debugPage: vi.fn(async (_cursor: DebugCursor) => ({} as DebugPage)),
    setDebugLevel: vi.fn(async (_level: DebugLevel) => 'INFO' as DebugLevel), status: vi.fn(async () => ({} as WifiStatus)),
    scanDetailed: vi.fn(async () => ({ networks: [] as WifiNetwork[], rawCount: 0 })), scan: vi.fn(async () => [] as WifiNetwork[]), listKnownWifi: vi.fn(async () => [] as WifiNetwork[]),
    retryWifi: vi.fn(async () => ({} as WifiStatus)), connectWifi: vi.fn(async (_ssid: string, _password: string) => ({} as WifiStatus)),
    calibrationCapabilities: vi.fn(async (_sensor: string) => ({} as BleCalibrationResponse)),
    calibrationStatus: vi.fn(async (_sensor: string) => ({} as BleCalibrationResponse)),
    calibrationEvidence: vi.fn(async (_sensor: string) => ({} as BleCalibrationResponse)),
    calibrationStart: vi.fn(async (_sensor: string, _parameters?: Record<string, string | number | boolean>) => ({} as BleCalibrationResponse)),
    calibrationCriteria: vi.fn(async (_sensor: string, _parameters: Record<string, string | number | boolean>) => ({} as BleCalibrationResponse)),
    calibrationCapture: vi.fn(async (_sensor: string, _anchor: 'tare' | 'reference' | 'empty' | 'full') => ({} as BleCalibrationResponse)),
    calibrationValidate: vi.fn(async (_sensor: string) => ({} as BleCalibrationResponse)),
    calibrationPreview: vi.fn(async (_sensor: string) => ({} as BleCalibrationResponse)),
    calibrationCommit: vi.fn(async (_sensor: string, _expected?: Record<string, string | number | boolean>) => ({} as BleCalibrationResponse)),
    calibrationCancel: vi.fn(async () => ({} as BleCalibrationResponse)),
    calibrationHistory: vi.fn(async (_sensor?: string) => ({} as BleCalibrationResponse)),
    calibrationRollback: vi.fn(async (_sensor: string, _version: string) => ({} as BleCalibrationResponse)),
    ...overrides,
  };
  return client;
}
function setup(expected: string | null = ID, configure: (options: BleProvisioningClientOptions) => Partial<PublicBleClient> = () => ({})) {
  let client!: PublicBleClient;
  let optionCopy!: BleProvisioningClientOptions;
  const session = new DeviceBleSession(expected, { createClient: (options) => {
    optionCopy = options; client = fakeClient(options, configure(options)); return client;
  } });
  return { session, get client() { return client; }, get options() { return optionCopy; } };
}

describe('DeviceBleSession', () => {
  it('constructs one client with only the approved read deadlines and runs all owners on it', async () => {
    const { session, client, options } = setup();
    await session.connect();
    await session.run('wifi', (c) => c.status());
    await session.run('debug', (c) => c.debugMeta());
    await session.calibrationAdapter.calibrationEvidence?.('mq7');
    expect(client.status).toHaveBeenCalledTimes(1);
    expect(options).toMatchObject({ readOnlyMaxAttempts: 3, commandTimeoutMs: { GET_DEVICE_ID: 30000, GET_STATUS: 30000, SCAN_WIFI: 30000, GET_DEBUG: 30000 } });
    expect(options.requestTimeoutMs).toBeUndefined();
  });
  it('rejects missing expected identity before client construction or chooser', async () => {
    let factory = vi.fn(() => fakeClient({}));
    const s = new DeviceBleSession(null, { createClient: factory });
    await expect(s.connect()).rejects.toThrow(); expect(factory).toHaveBeenCalledTimes(1);
    expect((factory.mock.results[0].value as PublicBleClient).connect).not.toHaveBeenCalled();
  });
  it('rejects an identity mismatch and clears routing context', async () => {
    const { session } = setup(ID, (o) => ({ verifyDeviceIdentity: vi.fn(async () => { o.onVerifiedDeviceIdentityChange?.('0000000000000002'); throw Error('mismatch'); }) }));
    await expect(session.connect()).rejects.toThrow(); expect(session.getSnapshot().deviceId).toBeNull();
  });
  it('exposes stable immutable snapshots and isolates subscriber exceptions', async () => {
    const { session } = setup(); const initial = session.getSnapshot();
    expect(session.getSnapshot()).toBe(initial);
    const listener = vi.fn(() => { throw Error('subscriber'); }); session.subscribe(listener);
    await session.connect(); expect(listener).toHaveBeenCalled(); expect(session.getSnapshot()).not.toBe(initial);
    expect(Object.isFrozen(session.getSnapshot())).toBe(true);
  });
  it('rejects overlapping runs and connect while work is reserved', async () => {
    const { session } = setup(); await session.connect(); const d = deferred<number>();
    const first = session.run('wifi', () => d.promise);
    await expect(session.run('debug', async () => 2)).rejects.toThrow();
    await expect(session.connect()).rejects.toThrow(); d.resolve(1); await first;
  });
  it('close preserves subscriptions and HTTPS leases and permits reconnect', async () => {
    const { session, client } = setup(); await session.connect(); const release = session.beginControlActivity('https-calibration');
    session.close(); expect(session.getSnapshot().controlActive).toBe(true); expect(client.close).toHaveBeenCalled();
    release(); await session.connect(); expect(session.getSnapshot().connected).toBe(true);
  });
  it('isolates stale resolve and reject from newer generation work', async () => {
    for (const rejectOld of [false, true]) {
      const { session } = setup(); await session.connect(); const old = deferred<number>();
      const oldRun = session.run('wifi', () => old.promise); session.close(); await session.connect();
      const newer = deferred<number>(); const newRun = session.run('debug', () => newer.promise);
      if (rejectOld) old.reject(Error('stale')); else old.resolve(1);
      await expect(oldRun).rejects.toThrow(); expect(session.getSnapshot().busy).toBe('debug');
      newer.resolve(3); await newRun; expect(session.getSnapshot().busy).toBeNull();
    }
  });
  it('disposal is terminal and invalidates subscriptions, leases and work', async () => {
    const { session, client } = setup(); await session.connect(); const release = session.beginControlActivity('https-calibration');
    const pending = deferred<void>(); const work = session.run('wifi', () => pending.promise);
    session.dispose(); release(); pending.resolve(); await expect(work).rejects.toThrow();
    await expect(session.connect()).rejects.toThrow(); expect(client.connect).toHaveBeenCalledTimes(1);
    expect(session.getSnapshot().controlActive).toBe(false);
  });
  it('uses unique idempotent leases and BLE close never releases HTTPS activity', async () => {
    const { session } = setup(); await session.connect(); const ble = session.beginControlActivity('ble-calibration'); const https = session.beginControlActivity('https-calibration');
    ble(); ble(); expect(session.getSnapshot().controlActive).toBe(true); session.close();
    expect(session.getSnapshot().controlActive).toBe(true); https(); https(); expect(session.getSnapshot().controlActive).toBe(false);
  });
  it('blocks debug while any control lease is held', async () => {
    const { session } = setup(); await session.connect(); const release = session.beginControlActivity('https-calibration');
    await expect(session.run('debug', async () => 1)).rejects.toThrow(); release(); await expect(session.run('debug', async () => 1)).resolves.toBe(1);
  });
  it('handles synchronous old close callbacks during deliberate reconnect', async () => {
    let opts!: BleProvisioningClientOptions;
    const fakeFactory = (options: BleProvisioningClientOptions) => {
      opts = options;
      return fakeClient(options, { connect: vi.fn(async () => { options.onConnectionChange?.(false); options.onConnectionChange?.(true); }),
        verifyDeviceIdentity: vi.fn(async (id: string) => id) });
    };
    const session = new DeviceBleSession(ID, { createClient: fakeFactory });
    await session.connect(); const generation = session.getSnapshot().generation;
    await session.connect();
    expect(session.getSnapshot().connected).toBe(true);
    expect(session.getSnapshot().generation).toBe(generation + 1);
    expect(opts.onConnectionChange).toBeDefined();
  });
  it('does not let a closed pending connect reject over a newer pending attempt', async () => {
    const connects: ReturnType<typeof deferred<void>>[] = [];
    const verifies: ReturnType<typeof deferred<string>>[] = [];
    const { session, client } = setup(ID, (sessionOptions) => ({
      connect: vi.fn(() => { const d = deferred<void>(); connects.push(d); return d.promise.then(() => { sessionOptions.onConnectionChange?.(true); }); }),
      verifyDeviceIdentity: vi.fn(() => { const d = deferred<string>(); verifies.push(d); return d.promise; }),
    }));
    const a = session.connect(); connects[0].resolve(); await vi.waitFor(() => expect(verifies).toHaveLength(1));
    session.close(); const b = session.connect();
    connects[1].resolve(); await vi.waitFor(() => expect(verifies).toHaveLength(2));
    verifies[0].resolve(ID); await expect(a).rejects.toThrow();
    expect(client.close).toHaveBeenCalledTimes(1);
    expect(session.getSnapshot()).toMatchObject({ busy: 'connect', connected: true, deviceId: null });
    verifies[1].resolve(ID); await b;
    expect(session.getSnapshot()).toMatchObject({ busy: null, connected: true, deviceId: ID });
  });
  it('does not let a late connect rejection close a newer succeeded attempt', async () => {
    const connects: ReturnType<typeof deferred<void>>[] = [];
    const { session, client } = setup(ID, (options) => ({
      connect: vi.fn(() => { const d = deferred<void>(); connects.push(d); return d.promise.then(() => options.onConnectionChange?.(true)); }),
    }));
    const a = session.connect(); session.close(); const b = session.connect();
    connects[1].resolve(); await b;
    connects[0].reject(Error('late old connect')); await expect(a).rejects.toThrow();
    expect(client.close).toHaveBeenCalledTimes(1);
    expect(session.getSnapshot()).toMatchObject({ busy: null, connected: true, deviceId: ID });
  });
  it('rejects a late connect resolution while a newer attempt is still pending', async () => {
    const connects: ReturnType<typeof deferred<void>>[] = [];
    const { session, client } = setup(ID, () => ({
      connect: vi.fn(() => { const d = deferred<void>(); connects.push(d); return d.promise; }),
    }));
    const a = session.connect(); session.close(); const b = session.connect();
    connects[0].resolve(); await expect(a).rejects.toThrow();
    expect(client.verifyDeviceIdentity).toHaveBeenCalledTimes(0);
    expect(client.close).toHaveBeenCalledTimes(1);
    expect(session.getSnapshot()).toMatchObject({ busy: 'connect', connected: false, deviceId: null });
    connects[1].resolve(); await b;
    expect(client.verifyDeviceIdentity).toHaveBeenCalledTimes(1);
    expect(session.getSnapshot()).toMatchObject({ busy: null, connected: true, deviceId: ID });
  });
  it('rejects a late connect resolution after a newer attempt succeeds', async () => {
    const connects: ReturnType<typeof deferred<void>>[] = [];
    const { session, client } = setup(ID, () => ({
      connect: vi.fn(() => { const d = deferred<void>(); connects.push(d); return d.promise; }),
    }));
    const a = session.connect(); session.close(); const b = session.connect();
    connects[1].resolve(); await b;
    connects[0].resolve(); await expect(a).rejects.toThrow();
    expect(client.verifyDeviceIdentity).toHaveBeenCalledTimes(1);
    expect(client.close).toHaveBeenCalledTimes(1);
    expect(session.getSnapshot()).toMatchObject({ busy: null, connected: true, deviceId: ID });
  });
  it('does not continue verification after disposal during connect', async () => {
    const connect = deferred<void>();
    const { session, client } = setup(ID, () => ({ connect: vi.fn(() => connect.promise) }));
    const pending = session.connect(); session.dispose(); connect.resolve();
    await expect(pending).rejects.toThrow();
    expect(client.verifyDeviceIdentity).not.toHaveBeenCalled();
    expect(client.close).toHaveBeenCalledTimes(1);
  });
  it('ignores an old identity result after the newer attempt has succeeded', async () => {
    const verifies: ReturnType<typeof deferred<string>>[] = [];
    const { session, client } = setup(ID, (options) => ({
      verifyDeviceIdentity: vi.fn(() => { const d = deferred<string>(); verifies.push(d); return d.promise; }),
    }));
    const a = session.connect(); await vi.waitFor(() => expect(verifies).toHaveLength(1));
    session.close(); const b = session.connect(); await vi.waitFor(() => expect(verifies).toHaveLength(2));
    verifies[1].resolve(ID); await b;
    verifies[0].resolve(ID); await expect(a).rejects.toThrow();
    expect(client.close).toHaveBeenCalledTimes(1);
    expect(session.getSnapshot()).toMatchObject({ busy: null, connected: true, deviceId: ID });
  });
  it('invalidates pending identity verification on a genuine disconnect', async () => {
    let options!: BleProvisioningClientOptions;
    const verify = deferred<string>();
    const { session, client } = setup(ID, (configured) => { options = configured; return { verifyDeviceIdentity: vi.fn(() => verify.promise) }; });
    const pending = session.connect(); await vi.waitFor(() => expect(client.verifyDeviceIdentity).toHaveBeenCalled());
    options.onConnectionChange?.(false); verify.resolve(ID);
    await expect(pending).rejects.toThrow();
    expect(session.getSnapshot()).toMatchObject({ connected: false, deviceId: null, busy: null });
    expect(client.close).toHaveBeenCalledTimes(0);
  });
  it('ignores late identity completion after close without starting more work', async () => {
    const verify = deferred<string>();
    const { session, client } = setup(ID, () => ({ verifyDeviceIdentity: vi.fn(() => verify.promise) }));
    const a = session.connect(); await Promise.resolve();
    session.close(); verify.resolve(ID); await expect(a).rejects.toThrow();
    expect(session.getSnapshot()).toMatchObject({ connected: false, deviceId: null, busy: null });
    expect(client.close).toHaveBeenCalledTimes(1);
  });
  it('treats an unexpected disconnect as a new BLE generation and releases only BLE leases', async () => {
    let options!: BleProvisioningClientOptions;
    const session = new DeviceBleSession(ID, { createClient: (o: BleProvisioningClientOptions) => { options = o; return fakeClient(o); } });
    await session.connect(); const ble = session.beginControlActivity('ble-calibration');
    const https = session.beginControlActivity('https-calibration'); const generation = session.getSnapshot().generation;
    options.onConnectionChange?.(false);
    expect(session.getSnapshot().generation).toBe(generation + 1);
    expect(session.getSnapshot().connected).toBe(false);
    expect(session.getSnapshot().controlActive).toBe(true);
    ble(); expect(session.getSnapshot().controlActive).toBe(true); https();
  });
  it('keeps an old lease release from clearing a newer lease', async () => {
    const { session } = setup(); const old = session.beginControlActivity('https-calibration'); old();
    const newer = session.beginControlActivity('https-calibration'); old();
    expect(session.getSnapshot().controlActive).toBe(true); newer();
    expect(session.getSnapshot().controlActive).toBe(false);
  });
  it('copies trace metadata and snapshots so caller mutation cannot affect later state', async () => {
    const { session, options } = setup();
    options.onDiagnosticEvent?.({ type: 'request_started', requestId: { password: 'secret' }, operation: 'GET_STATUS', elapsedMs: 4, payload: 'secret' } as unknown as BleClientDiagnosticEvent);
    const trace = session.getSnapshot().trace;
    expect(trace[0]).toEqual({ type: 'request_started', operation: 'GET_STATUS', elapsedMs: 4 });
    expect(Object.isFrozen(trace[0])).toBe(true); expect(Object.isFrozen(trace)).toBe(true);
  });
  it('caps trace at 60 and stores only allowlisted diagnostic metadata', async () => {
    const { session, options } = setup();
    for (let i = 0; i < 65; i++) options.onDiagnosticEvent?.({ type: 'request_started', operation: 'GET_STATUS', requestId: String(i), elapsedMs: i, reason: 'request_id_mismatch' } as BleClientDiagnosticEvent & { password: string });
    expect(session.getSnapshot().trace).toHaveLength(60);
    expect(JSON.stringify(session.getSnapshot().trace)).not.toMatch(/password|secret|SSID/);
    expect(session.getSnapshot().trace[59]).toMatchObject({ requestId: '64' });
  });
});
