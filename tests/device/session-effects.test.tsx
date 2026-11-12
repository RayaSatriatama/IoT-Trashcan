import { afterEach, describe, expect, it, vi } from 'vitest';
import type { CalibrationWizardProps } from '../../components/device/CalibrationWizard';

// This driver executes production hook callbacks, not React's DOM scheduler.
// State slots model the owning component; every instance has independent storage.
const runtime = vi.hoisted(() => ({
  current: null as any,
  routeId: 'route-a',
  device: null as any,
  clients: [] as any[],
}));
const depsChanged = (a: readonly unknown[] | undefined, b: readonly unknown[]) =>
  !a || a.length !== b.length || b.some((value, i) => !Object.is(value, a[i]));

vi.mock('react', async (importOriginal) => {
  const actual = await importOriginal<any>();
  return {
    ...actual,
    useState(initial: any) {
      const frame = runtime.current;
      const i = frame.stateIndex++;
      if (!(i in frame.state)) frame.state[i] = typeof initial === 'function' ? initial() : initial;
      return [frame.state[i], (value: any) => {
        if (!frame.mounted) return;
        const next = typeof value === 'function' ? value(frame.state[i]) : value;
        if (!Object.is(next, frame.state[i])) { frame.state[i] = next; frame.dirty = true; }
      }];
    },
    useRef(initial: any) {
      const frame = runtime.current;
      return frame.refs[frame.refIndex++] ??= { current: initial };
    },
    useMemo(create: () => unknown, deps: unknown[]) {
      const frame = runtime.current;
      const i = frame.memoIndex++;
      if (depsChanged(frame.memos[i]?.deps, deps)) frame.memos[i] = { deps, value: create() };
      return frame.memos[i].value;
    },
    useCallback(callback: unknown, deps: unknown[]) {
      const frame = runtime.current;
      const i = frame.memoIndex++;
      if (depsChanged(frame.memos[i]?.deps, deps)) frame.memos[i] = { deps, value: callback };
      return frame.memos[i].value;
    },
    useEffect(setup: () => unknown, deps: unknown[]) {
      const frame = runtime.current;
      const i = frame.effectIndex++;
      const old = frame.effects[i];
      if (depsChanged(old?.deps, deps)) {
        frame.effects[i] = { deps, setup, cleanup: old?.cleanup, changed: true };
      }
    },
    useSyncExternalStore: (_subscribe: unknown, get: () => unknown) => get(),
  };
});
vi.mock('expo-router', () => ({
  useLocalSearchParams: () => ({ id: runtime.routeId }),
  useRouter: () => ({ back: vi.fn(), push: vi.fn() }),
}));
vi.mock('react-native', () => ({ Image: () => null }));
vi.mock('tamagui', async (importOriginal) => {
  const actual = await importOriginal<any>();
  return { ...actual, useMedia: () => ({ tablet: false, wide: false }) };
});
vi.mock('../../lib/api/hooks', () => ({
  usePollingResource: () => ({
    data: runtime.current.resourceIndex++ === 0 ? runtime.device : null,
    loading: false, error: null, stale: false, retry: vi.fn(),
  }),
}));
vi.mock('../../lib/ble/client', () => ({
  BleProvisioningClient: vi.fn(function (options: any) {
    const client = {
      connect: vi.fn(async () => options.onConnectionChange?.(true)),
      close: vi.fn(() => options.onConnectionChange?.(false)),
      verifyDeviceIdentity: vi.fn(async (id: string) => id),
      status: vi.fn(async () => ({ state: 'CONNECTED', ssid: 'fixture' })),
      scanDetailed: vi.fn(async () => ({ networks: [], rawCount: 0 })),
      listKnownWifi: vi.fn(async () => []),
      debugMeta: vi.fn(),
      calibrationCapabilities: vi.fn(async (sensor: string) => ({ status: 'OK', sensor, data: { references: 'none', criteria: 'none', engineering: 'none' } })),
      calibrationEvidence: vi.fn(async (sensor: string) => ({ status: 'OK', sensor, data: { q: 'UNCALIBRATED', r: 'TEST_ONLY', s: '1', fr: 'FRESH', raw: '1' } })),
    };
    runtime.clients.push(client);
    return client;
  }),
}));

import { CalibrationWizard } from '../../components/device/CalibrationWizard';
import DeviceDetailRoute from '../../app/devices/[id]';
import { DeviceBleSession } from '../../lib/ble/device-session';
import { WifiSettingsController } from '../../lib/ble/wifi-settings';
import { DeviceDebugPanel } from '../../components/device/DeviceDebugPanel';
import { DeviceDetailSessionView } from '../../app/devices/[id]';
import { DebugViewController } from '../../lib/debug/poller';

type Effect = { setup: () => (() => void) | void; cleanup?: () => void; changed: boolean };
function lifecycle<Props>(Component: (props: Props) => unknown, initialProps: Props) {
  const frame = {
    state: [] as any[], refs: [] as any[], memos: [] as any[], effects: [] as Effect[],
    stateIndex: 0, refIndex: 0, memoIndex: 0, effectIndex: 0, resourceIndex: 0,
    dirty: false, mounted: true, output: null as unknown,
  };
  let props = initialProps;
  const render = (updates?: Partial<Props>) => {
    if (updates) props = { ...props, ...updates };
    let remaining = 20;
    do {
      if (--remaining === 0) throw new Error('Production effects did not settle');
      frame.stateIndex = frame.refIndex = frame.memoIndex = frame.effectIndex = frame.resourceIndex = 0;
      frame.dirty = false;
      runtime.current = frame;
      try { frame.output = Component(props); } finally { runtime.current = null; }
      // React cleans changed passive effects before running their new setups.
      const changed = frame.effects.filter((effect) => effect.changed);
      for (const effect of changed) { effect.cleanup?.(); effect.cleanup = undefined; }
      for (const effect of changed) {
        effect.changed = false;
        effect.cleanup = effect.setup() || undefined;
      }
    } while (frame.dirty);
  };
  const unmount = () => {
    if (!frame.mounted) return;
    frame.mounted = false;
    for (const effect of frame.effects) { effect.cleanup?.(); effect.cleanup = undefined; }
  };
  render();
  return { frame, render, unmount };
}

// CalibrationWizard's state slots, read from its actual hook declaration order.
const wizardState = { session: 2, busy: 3, phase: 16 } as const;
const wizardProps = (owner: DeviceBleSession): CalibrationWizardProps => ({
  deviceId: 'dev', sensorId: 'mq7', diagnostic: null, api: null,
  transport: 'https', onControlActivity: (source) => owner.beginControlActivity(source),
});
const device = (bleId: string | null) => ({
  device_code: 'unit', ble_device_id: bleId, slots: [], status: null,
  online_state: 'Offline', last_seen: null,
});
function routeOwners(frame: ReturnType<typeof lifecycle>['frame']) {
  return {
    owner: frame.memos.find((memo) => memo.value instanceof DeviceBleSession).value as DeviceBleSession,
    controller: frame.memos.find((memo) => memo.value instanceof WifiSettingsController).value as WifiSettingsController,
  };
}

afterEach(() => {
  vi.restoreAllMocks();
  vi.unstubAllGlobals();
  runtime.current = null;
  runtime.device = null;
  runtime.routeId = 'route-a';
  runtime.clients = [];
});

describe('actual Wizard activity effects', () => {
  it('waits for an admitted debug operation before one identity/capabilities/evidence read', async () => {
    const owner = new DeviceBleSession('dev'); await owner.connect();
    const client = runtime.clients[0]; let finish!: (value: unknown) => void;
    client.debugMeta.mockImplementation(() => new Promise(resolve => { finish = resolve; }));
    const pending = owner.run('debug', value => value.debugMeta());
    const wizard = lifecycle(CalibrationWizard, { ...wizardProps(owner), transport: 'ble', bleConnected: true, peerBusy: true, bleClient: owner.calibrationAdapter });
    wizard.frame.state[0] = true; wizard.render();
    expect(owner.getSnapshot().controlActive).toBe(true);
    expect(client.verifyDeviceIdentity).toHaveBeenCalledTimes(1);
    expect(client.calibrationCapabilities).not.toHaveBeenCalled();
    expect(client.calibrationEvidence).not.toHaveBeenCalled();
    finish({}); await pending; wizard.render({ peerBusy: false });
    for (let index = 0; index < 30; index++) await Promise.resolve();
    expect(client.verifyDeviceIdentity).toHaveBeenCalledTimes(2);
    expect(client.calibrationCapabilities).toHaveBeenCalledTimes(1);
    expect(client.calibrationEvidence).toHaveBeenCalledTimes(1);
    wizard.render();
    for (let index = 0; index < 10; index++) await Promise.resolve();
    expect(client.calibrationCapabilities).toHaveBeenCalledTimes(1);
    expect(client.calibrationEvidence).toHaveBeenCalledTimes(1);
    wizard.unmount(); owner.dispose();
  });

  it('does not start another read after the wizard closes during identity verification', async () => {
    const owner = new DeviceBleSession('dev'); await owner.connect();
    const client = runtime.clients[0]; let finish!: (id: string) => void;
    client.verifyDeviceIdentity.mockImplementation(() => new Promise(resolve => { finish = resolve; }));
    const wizard = lifecycle(CalibrationWizard, { ...wizardProps(owner), transport: 'ble', bleConnected: true, bleClient: owner.calibrationAdapter });
    wizard.frame.state[0] = true; wizard.render();
    wizard.frame.state[0] = false; wizard.render();
    expect(owner.getSnapshot().controlActive).toBe(true);
    finish('dev');
    for (let index = 0; index < 30; index++) await Promise.resolve();
    expect(client.calibrationCapabilities).not.toHaveBeenCalled();
    expect(client.calibrationEvidence).not.toHaveBeenCalled();
    wizard.render();
    expect(owner.getSnapshot().controlActive).toBe(false);
    wizard.unmount(); owner.dispose();
  });

  it('leases the BLE peer while the wizard is open before any read begins', () => {
    const owner = new DeviceBleSession('dev');
    const wizard = lifecycle(CalibrationWizard, { ...wizardProps(owner), transport: 'ble', bleConnected: true, peerBusy: true,
      bleClient: { verifyDeviceIdentity: vi.fn(), calibrationCapabilities: vi.fn(), calibrationEvidence: vi.fn(), calibrationStart: vi.fn(), calibrationCriteria: vi.fn(), calibrationCapture: vi.fn(), calibrationValidate: vi.fn(), calibrationPreview: vi.fn(), calibrationCommit: vi.fn(), calibrationCancel: vi.fn() } });
    wizard.frame.state[0] = true;
    wizard.render();
    expect(owner.getSnapshot().controlActive).toBe(true);
    wizard.frame.state[0] = false;
    wizard.render();
    expect(owner.getSnapshot().controlActive).toBe(false);
    wizard.unmount(); owner.dispose();
  });

  it('holds one lease through busy and nonterminal work, then releases at terminal state', async () => {
    const owner = new DeviceBleSession('dev');
    await owner.connect();
    const acquire = vi.spyOn(owner, 'beginControlActivity');
    const wizard = lifecycle(CalibrationWizard, wizardProps(owner));
    expect(acquire).not.toHaveBeenCalled();
    wizard.frame.state[wizardState.busy] = true;
    wizard.render();
    expect(acquire).toHaveBeenCalledTimes(1);
    await expect(owner.run('debug', (client) => client.debugMeta())).rejects.toThrow(/paused/i);
    wizard.frame.state[wizardState.busy] = false;
    wizard.frame.state[wizardState.session] = 'session-a';
    wizard.frame.state[wizardState.phase] = 'CAPTURING';
    wizard.render(); wizard.render();
    expect(acquire).toHaveBeenCalledTimes(1);
    expect(owner.getSnapshot().controlActive).toBe(true);
    wizard.frame.state[wizardState.phase] = 'COMMITTED';
    wizard.render();
    expect(owner.getSnapshot().controlActive).toBe(false);
    wizard.unmount(); owner.dispose();
  });

  it.each(['deviceId', 'sensorId', 'transport'] as const)(
    'keeps pending work protected through a %s context replacement', (field) => {
      const owner = new DeviceBleSession('dev');
      const acquire = vi.spyOn(owner, 'beginControlActivity');
      const wizard = lifecycle(CalibrationWizard, wizardProps(owner));
      wizard.frame.state[wizardState.busy] = true;
      wizard.render();
      expect(owner.getSnapshot().controlActive).toBe(true);
      const updates = field === 'deviceId' ? { deviceId: 'next-device' }
        : field === 'sensorId' ? { sensorId: 'mq4' as const } : { transport: 'ble' as const, bleConnected: true };
      wizard.render(updates);
      // Busy remains true until its pending operation settles; context reset is not completion.
      expect(wizard.frame.state[wizardState.busy]).toBe(true);
      expect(owner.getSnapshot().controlActive).toBe(true);
      expect(acquire).toHaveBeenLastCalledWith(field === 'transport' ? 'ble-calibration' : 'https-calibration');
      wizard.frame.state[wizardState.busy] = false;
      wizard.render();
      expect(owner.getSnapshot().controlActive).toBe(false);
      wizard.unmount(); owner.dispose();
    },
  );

  it('restores a saved session and retires its lease on terminal and unmount', () => {
    const storage = new Map([['sigap.calibration.dev.mq7', JSON.stringify({ sessionId: 'restored', commandId: 'cmd-a' })]]);
    vi.stubGlobal('window', { sessionStorage: {
      getItem: (key: string) => storage.get(key) ?? null,
      setItem: (key: string, value: string) => storage.set(key, value),
      removeItem: (key: string) => storage.delete(key),
    } });
    const owner = new DeviceBleSession('dev');
    const wizard = lifecycle(CalibrationWizard, wizardProps(owner));
    expect(wizard.frame.state[wizardState.session]).toBe('restored');
    expect(owner.getSnapshot().controlActive).toBe(true);
    wizard.frame.state[wizardState.phase] = 'EXPIRED';
    wizard.render();
    expect(owner.getSnapshot().controlActive).toBe(false);
    wizard.frame.state[wizardState.phase] = 'CAPTURING';
    wizard.render();
    expect(owner.getSnapshot().controlActive).toBe(true);
    wizard.unmount(); wizard.unmount();
    expect(owner.getSnapshot().controlActive).toBe(false);
    owner.dispose();
  });

  it('retires an old nonbusy session on context change without releasing the next session', () => {
    const owner = new DeviceBleSession('dev');
    const wizard = lifecycle(CalibrationWizard, wizardProps(owner));
    wizard.frame.state[wizardState.session] = 'old-session';
    wizard.frame.state[wizardState.phase] = 'CAPTURING';
    wizard.render();
    const oldCleanup = wizard.frame.effects[0].cleanup!;
    expect(owner.getSnapshot().controlActive).toBe(true);
    wizard.render({ sensorId: 'mq4' });
    expect(wizard.frame.state[wizardState.session]).toBeNull();
    expect(owner.getSnapshot().controlActive).toBe(false);
    wizard.frame.state[wizardState.session] = 'new-session';
    wizard.frame.state[wizardState.phase] = 'CAPTURING';
    wizard.render();
    oldCleanup(); oldCleanup();
    expect(owner.getSnapshot().controlActive).toBe(true);
    wizard.unmount();
    expect(owner.getSnapshot().controlActive).toBe(false);
    owner.dispose();
  });

  it('does not let stale HTTPS cleanup release the replacement BLE activity lease', () => {
    const owner = new DeviceBleSession('dev');
    const wizard = lifecycle(CalibrationWizard, wizardProps(owner));
    wizard.frame.state[wizardState.busy] = true;
    wizard.render();
    const oldCleanup = wizard.frame.effects[0].cleanup!;
    wizard.render({ transport: 'ble', bleConnected: true });
    oldCleanup();
    expect(owner.getSnapshot().controlActive).toBe(true);
    wizard.frame.state[wizardState.busy] = false;
    wizard.render();
    expect(owner.getSnapshot().controlActive).toBe(false);
    wizard.unmount(); owner.dispose();
  });

  it('keeps HTTPS activity when the real BLE disconnect effect clears its peer session', () => {
    const owner = new DeviceBleSession('dev');
    const https = lifecycle(CalibrationWizard, wizardProps(owner));
    const ble = lifecycle(CalibrationWizard, { ...wizardProps(owner), transport: 'ble', bleConnected: true });
    for (const instance of [https, ble]) {
      instance.frame.state[wizardState.session] = 'active';
      instance.frame.state[wizardState.phase] = 'CAPTURING';
      instance.render();
    }
    owner.close();
    ble.render({ bleConnected: false });
    expect(ble.frame.state[wizardState.session]).toBeNull();
    expect(owner.getSnapshot().controlActive).toBe(true);
    ble.unmount();
    expect(owner.getSnapshot().controlActive).toBe(true);
    https.unmount();
    expect(owner.getSnapshot().controlActive).toBe(false);
    owner.dispose();
  });
});

describe('actual route owner effects', () => {
  it('keeps idle Wi-Fi open from pausing visible debug and offers disconnect for a stalled read', async () => {
    runtime.device = device('canonical-a');
    const route = lifecycle(DeviceDetailRoute, {});
    const { owner, controller } = routeOwners(route.frame);
    const client = runtime.clients[0];
    let finishDebug!: (value: unknown) => void;
    client.debugMeta.mockImplementation(() => new Promise(resolve => { finishDebug = resolve; }));
    await owner.connect();
    client.listKnownWifi.mockResolvedValue([{ ssid: 'Saved-net', rssi: -48, secure: true }]);
    await controller.loadStatus(); await controller.loadSaved();
    controller.open(); route.render();
    expect(owner.getSnapshot()).toMatchObject({ connected: true, deviceId: 'canonical-a', busy: null });
    const debugController = new DebugViewController({ deviceId: 'unit', session: owner });
    const view = lifecycle(DeviceDetailSessionView, { owner, controller, deviceCode: 'unit', bleDeviceId: 'canonical-a', sensorId: 'mq7', onSensorChange: vi.fn(), diagnostic: null, onDiagnosticRefresh: vi.fn() });
    const debugNode = (function find(node: any): any {
      if (!node) return null;
      if (node.type === DeviceDebugPanel) return node;
      const children = node.props?.children;
      for (const child of Array.isArray(children) ? children : [children]) {
        const found = find(child);
        if (found) return found;
      }
      return null;
    })(view.frame.output);
    expect(debugNode?.props.busy).toBe(false);
    debugController.setVisible(true);
    expect(client.debugMeta).toHaveBeenCalledTimes(1);
    expect(owner.getSnapshot().busy).toBe('debug');
    expect(client.connect).toHaveBeenCalledTimes(1);
    expect(controller.getSnapshot()).toMatchObject({ open: true, status: { state: 'CONNECTED' }, saved: [{ ssid: 'Saved-net' }] });
    const disconnect = vi.spyOn(owner, 'close');
    owner.close();
    expect(disconnect).toHaveBeenCalledTimes(1);
    expect(owner.getSnapshot()).toMatchObject({ connected: false, deviceId: null, busy: null });
    finishDebug({});
    for (let index = 0; index < 8; index++) await Promise.resolve();
    expect(client.debugMeta).toHaveBeenCalledTimes(1);
    expect(client.connect).toHaveBeenCalledTimes(1);
    route.unmount(); view.unmount();
  });
  it('reuses a resolved owner and disposes old controller before session on identity, route and unmount', async () => {
    const order: string[] = [];
    const disposeController = WifiSettingsController.prototype.dispose;
    const disposeOwner = DeviceBleSession.prototype.dispose;
    vi.spyOn(WifiSettingsController.prototype, 'dispose').mockImplementation(function (this: WifiSettingsController) {
      order.push('controller'); disposeController.call(this);
    });
    vi.spyOn(DeviceBleSession.prototype, 'dispose').mockImplementation(function (this: DeviceBleSession) {
      order.push('session'); disposeOwner.call(this);
    });
    const route = lifecycle(DeviceDetailRoute, {});
    const missing = routeOwners(route.frame);
    await expect(missing.owner.connect()).rejects.toThrow(/identity/i);
    expect(runtime.clients[0].connect).not.toHaveBeenCalled();
    runtime.device = device('canonical-a');
    route.render();
    const first = routeOwners(route.frame);
    expect(first.owner).not.toBe(missing.owner);
    expect(order).toEqual(['controller', 'session']);
    expect(runtime.clients[0].close).toHaveBeenCalledTimes(1);
    await first.owner.connect();
    runtime.device = { ...runtime.device };
    route.render();
    expect(routeOwners(route.frame)).toEqual(first);
    expect(runtime.clients).toHaveLength(2);
    let resolve!: (value: any) => void;
    runtime.clients[1].status.mockImplementation(() => new Promise((yes) => { resolve = yes; }));
    const oldOperation = first.controller.loadStatus();
    runtime.device = device('canonical-b');
    route.render();
    const second = routeOwners(route.frame);
    second.controller.setPassword('fresh');
    resolve({ state: 'CONNECTED', ssid: 'stale' });
    await oldOperation;
    expect(second.controller.getSnapshot()).toMatchObject({ password: 'fresh', status: null });
    expect(runtime.clients[1].close).toHaveBeenCalledTimes(1);
    runtime.routeId = 'route-b';
    route.render();
    expect(routeOwners(route.frame).owner).not.toBe(second.owner);
    expect(runtime.clients[2].close).toHaveBeenCalledTimes(1);
    route.unmount(); route.unmount();
    expect(order).toEqual(['controller', 'session', 'controller', 'session', 'controller', 'session', 'controller', 'session']);
    expect(runtime.clients[3].close).toHaveBeenCalledTimes(1);
    expect(runtime.clients[3].connect).not.toHaveBeenCalled();
  });
});
