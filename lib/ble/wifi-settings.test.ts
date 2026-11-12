import { describe, expect, it, vi } from 'vitest';
import type { BleCalibrationResponse, DebugCursor, DebugLevel, DebugMeta, DebugPage, WifiNetwork, WifiStatus } from './protocol';
import type { BleProvisioningClientOptions } from './client';
import { DeviceBleSession, type PublicBleClient } from './device-session';
import { WifiSettingsController } from './wifi-settings';

const ID = '0000000000000001';
const deferred = <T,>() => { let resolve!: (value: T) => void; let reject!: (reason?: unknown) => void; const promise = new Promise<T>((yes, no) => { resolve = yes; reject = no; }); return { promise, resolve, reject }; };
function fake(options: BleProvisioningClientOptions, overrides: Partial<PublicBleClient> = {}): PublicBleClient {
  return {
    connect: vi.fn(async () => { options.onConnectionChange?.(true); }), close: vi.fn(() => options.onConnectionChange?.(false)),
    verifyDeviceIdentity: vi.fn(async (id) => id), get verifiedDeviceId() { return ID; },
    debugMeta: vi.fn(async () => ({} as DebugMeta)), debugPage: vi.fn(async (_c: DebugCursor) => ({} as DebugPage)),
    setDebugLevel: vi.fn(async (_l: DebugLevel) => 'INFO' as DebugLevel), status: vi.fn(async () => ({ state: 'CONNECTED', ssid: 'Lab' } as WifiStatus)),
    scanDetailed: vi.fn(async () => ({ networks: [{ ssid: 'Lab', rssi: -40, secure: true }], rawCount: 3 })), scan: vi.fn(async () => [] as WifiNetwork[]),
    listKnownWifi: vi.fn(async () => [] as WifiNetwork[]), retryWifi: vi.fn(async () => ({ state: 'CONNECTING', ssid: null } as WifiStatus)),
    connectWifi: vi.fn(async () => ({ state: 'CONNECTED', ssid: 'Lab' } as WifiStatus)),
    calibrationCapabilities: vi.fn(async (_s) => ({} as BleCalibrationResponse)), calibrationStatus: vi.fn(async (_s) => ({} as BleCalibrationResponse)),
    calibrationEvidence: vi.fn(async (_s) => ({} as BleCalibrationResponse)), calibrationStart: vi.fn(async (_s, _p) => ({} as BleCalibrationResponse)),
    calibrationCriteria: vi.fn(async (_s, _p) => ({} as BleCalibrationResponse)), calibrationCapture: vi.fn(async (_s, _a) => ({} as BleCalibrationResponse)),
    calibrationValidate: vi.fn(async (_s) => ({} as BleCalibrationResponse)), calibrationPreview: vi.fn(async (_s) => ({} as BleCalibrationResponse)),
    calibrationCommit: vi.fn(async (_s, _p) => ({} as BleCalibrationResponse)), calibrationCancel: vi.fn(async () => ({} as BleCalibrationResponse)),
    calibrationHistory: vi.fn(async (_s) => ({} as BleCalibrationResponse)), calibrationRollback: vi.fn(async (_s, _v) => ({} as BleCalibrationResponse)),
    ...overrides,
  };
}
function setup(overrides: Partial<PublicBleClient> = {}) {
  let client!: PublicBleClient;
  const session = new DeviceBleSession(ID, { createClient: (options) => client = fake(options, overrides) });
  const controller = new WifiSettingsController(session);
  return { session, controller, get client() { return client; } };
}

describe('WifiSettingsController', () => {
  it('uses the shared session and exposes raw scan count', async () => {
    const { session, controller, client } = setup(); await session.connect(); await controller.scan();
    expect(client.scanDetailed).toHaveBeenCalledTimes(1);
    expect(controller.getSnapshot()).toMatchObject({ nearby: [{ ssid: 'Lab', rssi: -40, secure: true }], rawCount: 3, selectedSsid: 'Lab' });
  });

  it('marks only pending Wi-Fi reads recoverable and retires late reads on disconnect', async () => {
    const timedOut = deferred<WifiStatus>();
    const late = deferred<{ networks: WifiNetwork[]; rawCount: number }>();
    const { session, controller, client } = setup({
      status: vi.fn(() => timedOut.promise), scanDetailed: vi.fn(() => late.promise),
    });
    await session.connect();
    controller.selectSsid('Lab'); controller.setPassword('local-only');
    const mutation = controller.submit();
    expect(controller.getSnapshot()).toMatchObject({ busy: true, readonlyBusy: false, password: '' });
    await mutation;
    const statusRead = controller.loadStatus();
    expect(controller.getSnapshot()).toMatchObject({ busy: true, readonlyBusy: true });
    timedOut.reject(new Error('request timeout'));
    await statusRead;
    expect(controller.getSnapshot()).toMatchObject({ busy: false, readonlyBusy: false, error: 'request timeout' });
    const scan = controller.scan();
    expect(controller.getSnapshot().readonlyBusy).toBe(true);
    session.close();
    late.resolve({ networks: [{ ssid: 'Late', rssi: -1, secure: false }], rawCount: 1 });
    await scan;
    expect(controller.getSnapshot()).toMatchObject({ busy: false, readonlyBusy: false, nearby: [], rawCount: 0 });
    expect(client.connect).toHaveBeenCalledTimes(1);
  });

  it('keeps readonly recovery available across close/reopen until the active request settles', async () => {
    const pending = deferred<{ networks: WifiNetwork[]; rawCount: number }>();
    const { session, controller } = setup({ scanDetailed: vi.fn(() => pending.promise) });
    await session.connect(); controller.open();
    const scan = controller.scan();
    controller.close(); controller.open();
    expect(controller.getSnapshot().readonlyBusy).toBe(true);
    expect(session.getSnapshot().busy).toBe('wifi');
    pending.resolve({ networks: [], rawCount: 0 }); await scan;
    expect(controller.getSnapshot()).toMatchObject({ busy: false, readonlyBusy: false });
  });
  it('does not let a rejected readonly overlap relabel an active mutation', async () => {
    const mutationRequest = deferred<WifiStatus>();
    const { session, controller } = setup({ connectWifi: vi.fn(() => mutationRequest.promise) });
    await session.connect(); controller.selectSsid('Lab');
    const mutation = controller.submit();
    await controller.scan();
    expect(controller.getSnapshot()).toMatchObject({ busy: true, readonlyBusy: false });
    controller.close(); controller.open();
    expect(controller.getSnapshot()).toMatchObject({ busy: true, readonlyBusy: false });
    mutationRequest.resolve({ state: 'CONNECTED', ssid: 'Lab' }); await mutation;
  });
  it('clears password synchronously before one mutation and preserves newer typing', async () => {
    const request = deferred<WifiStatus>(); const { session, controller, client } = setup({ connectWifi: vi.fn(() => request.promise) });
    await session.connect(); controller.selectSsid('Lab'); controller.setPassword('old');
    const pending = controller.submit(); expect(controller.getSnapshot().password).toBe('');
    controller.setPassword('new'); request.resolve({ state: 'CONNECTED', ssid: 'Lab' }); await pending;
    expect(controller.getSnapshot().password).toBe('new'); expect(client.connectWifi).toHaveBeenCalledTimes(1);
  });
  it.each(['scan', 'status', 'saved', 'submit', 'retry'] as const)('%s cannot commit after close/reopen when it resolves or rejects late', async (operation) => {
    for (const ending of ['resolve', 'reject'] as const) {
      const pending = deferred<unknown>();
      const old = { ssid: 'Old', rssi: -70, secure: true };
      const overrides: Partial<PublicBleClient> = {
        scanDetailed: vi.fn(() => pending.promise as never), status: vi.fn(() => pending.promise as never),
        listKnownWifi: vi.fn(() => pending.promise as never), connectWifi: vi.fn(() => pending.promise as never),
        retryWifi: vi.fn(() => pending.promise as never),
      };
      const { session, controller } = setup(overrides);
      await session.connect(); controller.open(); controller.selectSsid('Current');
      const pendingOp = operation === 'scan' ? controller.scan() : operation === 'status' ? controller.loadStatus()
        : operation === 'saved' ? controller.loadSaved() : operation === 'submit' ? controller.submit() : controller.retrySaved();
      if (operation === 'submit') controller.setPassword('submitted');
      controller.close(); await session.close(); await session.connect(); controller.open(); controller.setPassword('current-secret'); controller.selectSsid('Current');
      if (ending === 'resolve') pending.resolve(operation === 'scan' ? { networks: [old], rawCount: 91 }
        : operation === 'saved' ? [old] : { state: 'CONNECTED', ssid: 'Old' });
      else pending.reject(Error('stale rejection'));
      await pendingOp;
      expect(controller.getSnapshot()).toMatchObject({ status: null, nearby: [], saved: [], rawCount: 0,
        selectedSsid: 'Current', password: 'current-secret', busy: false, error: null, success: null });
    }
  });
  it.each(['scan', 'status', 'saved', 'submit', 'retry'] as const)('%s cannot commit after unexpected disconnect/reconnect when it resolves or rejects late', async (operation) => {
    for (const ending of ['resolve', 'reject'] as const) {
      const pending = deferred<unknown>(); const old = { ssid: 'Old', rssi: -70, secure: true };
      const overrides: Partial<PublicBleClient> = {
        scanDetailed: vi.fn(() => pending.promise as never), status: vi.fn(() => pending.promise as never),
        listKnownWifi: vi.fn(() => pending.promise as never), connectWifi: vi.fn(() => pending.promise as never), retryWifi: vi.fn(() => pending.promise as never),
      };
      const { session, controller } = setup(overrides); await session.connect(); controller.open(); controller.selectSsid('Current');
      if (operation === 'submit') controller.setPassword('submitted');
      const pendingOp = operation === 'scan' ? controller.scan() : operation === 'status' ? controller.loadStatus()
        : operation === 'saved' ? controller.loadSaved() : operation === 'submit' ? controller.submit() : controller.retrySaved();
      const generation = session.getSnapshot().generation;
      session.close(); await session.connect(); expect(session.getSnapshot().generation).toBeGreaterThan(generation);
      controller.setPassword('current-secret'); controller.selectSsid('Current');
      if (ending === 'resolve') pending.resolve(operation === 'scan' ? { networks: [old], rawCount: 92 }
        : operation === 'saved' ? [old] : { state: 'CONNECTED', ssid: 'Old' });
      else pending.reject(Error('stale rejection'));
      await pendingOp;
      expect(controller.getSnapshot()).toMatchObject({ status: null, nearby: [], saved: [], rawCount: 0,
        selectedSsid: 'Current', password: 'current-secret', busy: false, error: null, success: null });
    }
  });
  it.each(['scan', 'status', 'saved', 'submit', 'retry'] as const)('%s completing after dispose cannot publish or mutate controller state', async (operation) => {
    for (const ending of ['resolve', 'reject'] as const) {
      const pending = deferred<unknown>(); const overrides: Partial<PublicBleClient> = {
        scanDetailed: vi.fn(() => pending.promise as never), status: vi.fn(() => pending.promise as never),
        listKnownWifi: vi.fn(() => pending.promise as never), connectWifi: vi.fn(() => pending.promise as never), retryWifi: vi.fn(() => pending.promise as never),
      };
      const { session, controller } = setup(overrides); await session.connect(); controller.open(); controller.selectSsid('Current');
      if (operation === 'submit') controller.setPassword('submitted');
      const pendingOp = operation === 'scan' ? controller.scan() : operation === 'status' ? controller.loadStatus()
        : operation === 'saved' ? controller.loadSaved() : operation === 'submit' ? controller.submit() : controller.retrySaved();
      controller.dispose(); const notifications = vi.fn(); controller.subscribe(notifications); const disposed = controller.getSnapshot();
      if (ending === 'resolve') pending.resolve(operation === 'scan' ? { networks: [{ ssid: 'Stale', rssi: -1, secure: false }], rawCount: 77 }
        : operation === 'saved' ? [{ ssid: 'Stale', rssi: -1, secure: false }] : { state: 'CONNECTED', ssid: 'Stale' });
      else pending.reject(Error('late disposed failure'));
      await pendingOp; controller.setPassword('must-not-return'); controller.open(); controller.selectSsid('after-dispose');
      expect(controller.getSnapshot()).toBe(disposed); expect(controller.getSnapshot().password).toBe(''); expect(notifications).not.toHaveBeenCalled();
    }
  });
  it('does not let a busy-blocked connect invalidate active Wi-Fi work', async () => {
    const request = deferred<WifiStatus>();
    const { session, controller } = setup({ status: vi.fn(() => request.promise) });
    await session.connect();
    const pending = controller.loadStatus();
    await expect(controller.connectBle()).rejects.toThrow('already running');
    await expect(controller.connectBle()).rejects.toThrow('already running');
    expect(controller.getSnapshot().busy).toBe(true);
    request.resolve({ state: 'CONNECTED', ssid: 'Lab' });
    await pending;
    expect(controller.getSnapshot().status).toEqual({ state: 'CONNECTED', ssid: 'Lab' });
  });
  it('reports failed admitted reconnect despite its own synchronous old-close callback', async () => {
    let options!: BleProvisioningClientOptions;
    let client!: PublicBleClient;
    const session = new DeviceBleSession(ID, { createClient: (o) => {
      options = o;
      client = fake(o, { connect: vi.fn(async () => { o.onConnectionChange?.(false); throw Error('radio unavailable'); }) });
      return client;
    } });
    const controller = new WifiSettingsController(session);
    (client.connect as ReturnType<typeof vi.fn>).mockImplementationOnce(async () => { options.onConnectionChange?.(true); });
    await session.connect();
    await expect(controller.connectBle()).rejects.toThrow('radio unavailable');
    expect(controller.getSnapshot()).toMatchObject({ error: 'radio unavailable', busy: false, password: '' });
    expect(session.getSnapshot()).toMatchObject({ connected: false, deviceId: null, busy: null });
    (client.connect as ReturnType<typeof vi.fn>).mockImplementationOnce(async () => { options.onConnectionChange?.(false); options.onConnectionChange?.(true); });
    await controller.connectBle();
    expect(session.getSnapshot()).toMatchObject({ connected: true, deviceId: ID, busy: null });
    expect(client.connect).toHaveBeenCalledTimes(3);
  });
  it('clears the current snapshot on dispose without mutating held snapshots', async () => {
    const { session, controller } = setup(); await session.connect();
    controller.setPassword('secret');
    const held = controller.getSnapshot();
    controller.dispose();
    expect(controller.getSnapshot().password).toBe('');
    expect(held.password).toBe('secret');
  });
  it('retains operation error after busy cleanup and retrySaved sends once', async () => {
    const { session, controller, client } = setup({ retryWifi: vi.fn(async () => { throw Error('offline'); }) });
    await session.connect(); await controller.retrySaved();
    expect(controller.getSnapshot()).toMatchObject({ busy: false, error: 'offline' });
    expect(client.retryWifi).toHaveBeenCalledTimes(1);
  });
  it('clears password on unexpected disconnect but close leaves shared GATT intact', async () => {
    let opts!: BleProvisioningClientOptions; const session = new DeviceBleSession(ID, { createClient: (o) => { opts = o; return fake(o); } });
    const controller = new WifiSettingsController(session); await session.connect(); controller.setPassword('secret'); controller.close();
    expect(session.getSnapshot().connected).toBe(true); controller.setPassword('secret'); opts.onConnectionChange?.(false);
    expect(controller.getSnapshot().password).toBe('');
  });
});
