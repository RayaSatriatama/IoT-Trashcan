import { afterEach, expect, it, vi } from 'vitest';

// Executes real owner hook callbacks with isolated frames, not ReactDOM scheduling.
const runtime = vi.hoisted(() => ({ current: null as any }));
vi.mock('react', async original => {
  const actual = await original<any>();
  const memo = (create: () => any, deps: any[]) => {
    const f = runtime.current, index = f.mi++, old = f.memos[index];
    if (!old || deps.length !== old.deps.length || deps.some((value, i) => !Object.is(value, old.deps[i])))
      f.memos[index] = { deps, value: create() };
    return f.memos[index].value;
  };
  return { ...actual, useMemo: memo, useCallback: (callback: any, deps: any[]) => memo(() => callback, deps),
    useState(initial: any) {
      const f = runtime.current, index = f.si++;
      if (!(index in f.state)) f.state[index] = typeof initial === 'function' ? initial() : initial;
      return [f.state[index], (value: any) => { f.state[index] = typeof value === 'function' ? value(f.state[index]) : value; }];
    },
    useSyncExternalStore: (_subscribe: any, get: any) => get(),
    useEffect(setup: any, deps: any[]) {
      const f = runtime.current, index = f.ei++, old = f.effects[index];
      if (!old || deps.length !== old.deps.length || deps.some((value, i) => !Object.is(value, old.deps[i])))
        f.effects[index] = { deps, setup, cleanup: old?.cleanup, changed: true };
    },
  };
});
vi.mock('expo-router', () => ({ useLocalSearchParams: () => ({}), useRouter: () => ({}) }));
vi.mock('react-native', () => ({ Image: () => null }));

import { DeviceDetailSessionView } from '../../app/devices/[id]';
import { CalibrationWizard } from '../../components/device/CalibrationWizard';
import { DeviceDebugPanel } from '../../components/device/DeviceDebugPanel';
import { DeviceBleSession } from '../../lib/ble/device-session';
import { WifiSettingsController } from '../../lib/ble/wifi-settings';
import { DebugViewController } from '../../lib/debug/poller';
import { SensorSelector } from '../../components/device/SensorSelector';

function mount(owner: DeviceBleSession) {
  const wifi = new WifiSettingsController(owner);
  const onSensorChange = vi.fn();
  const f = { ei: 0, mi: 0, si: 0, state: [] as any[], memos: [] as any[], effects: [] as any[], tree: null as any };
  let code = 'TEST-DEVICE';
  const render = (deviceCode = code) => {
    code = deviceCode; f.ei = f.mi = f.si = 0;
    runtime.current = f;
    try {
      f.tree = DeviceDetailSessionView({ owner, controller: wifi, deviceCode,
        bleDeviceId: 'canonical', sensorId: 'mq7', onSensorChange, diagnostic: null, onDiagnosticRefresh: vi.fn() });
    } finally { runtime.current = null; }
    const changed = f.effects.filter(effect => effect.changed);
    for (const effect of changed) { effect.cleanup?.(); effect.cleanup = undefined; }
    for (const effect of changed) { effect.changed = false; effect.cleanup = effect.setup(); }
  };
  const debug = () => f.memos.find(memo => memo.value instanceof DebugViewController).value as DebugViewController;
  const nodes = (): any[] => {
    const result: any[] = [];
    const walk = (node: any) => { if (Array.isArray(node)) node.forEach(walk); else if (node?.props) { result.push(node); walk(node.props.children); } };
    walk(f.tree); return result;
  };
  const panel = () => nodes().find((child: any) => child.type === DeviceDebugPanel);
  render();
  return { render, debug, panel, nodes, onSensorChange, unmount() { for (const effect of f.effects) { effect.cleanup?.(); effect.cleanup = undefined; } wifi.dispose(); } };
}
function ownerFixture() {
  const client: any = { connect: vi.fn(async () => {}), close: vi.fn(),
    verifyDeviceIdentity: vi.fn(async (id: string) => id), debugMeta: vi.fn(), debugPage: vi.fn() };
  return { owner: new DeviceBleSession('canonical', { createClient: () => client }), client };
}
afterEach(() => { vi.unstubAllGlobals(); vi.restoreAllMocks(); });

it('presents exactly one calibration wizard and locks transport while activity is live', () => {
  const { owner } = ownerFixture(), view = mount(owner);
  const wizards = () => view.nodes().filter(node => node.type === CalibrationWizard);
  expect(wizards()).toHaveLength(1);
  expect(wizards()[0].props.transport).toBe('ble');
  const remote = () => view.nodes().find(node => node.props['aria-label'] === 'Kalibrasi melalui HTTPS');
  remote().props.onPress(); view.render();
  expect(wizards()).toHaveLength(1);
  expect(wizards()[0].props.transport).toBe('https');
  const release = owner.beginControlActivity('https-calibration'); view.render();
  const local = view.nodes().find(node => node.props['aria-label'] === 'Kalibrasi melalui BLE');
  expect(local.props.disabled).toBe(true);
  local.props.onPress(); view.render();
  expect(wizards()[0].props.transport).toBe('https');
  release(); view.unmount(); owner.dispose();
});

it('forwards sensor selection but rejects a retained callback while calibration owns the peer', () => {
  const { owner } = ownerFixture(), view = mount(owner);
  const select = view.nodes().find(node => node.type === SensorSelector);
  select.props.onChange('mq4');
  expect(view.onSensorChange).toHaveBeenCalledExactlyOnceWith('mq4');
  const release = owner.beginControlActivity('https-calibration');
  select.props.onChange('mq7');
  expect(view.onSensorChange).toHaveBeenCalledTimes(1);
  release(); view.unmount(); owner.dispose();
});

it('real session-view owner replaces and disposes pending debug on device change and unmount', async () => {
  const { owner } = ownerFixture();
  const requests: Array<{ signal: AbortSignal; resolve: (value: Response) => void; url: string }> = [];
  vi.stubGlobal('fetch', vi.fn((url: string, options: any) => new Promise<Response>(resolve => {
    requests.push({ signal: options.signal, resolve, url });
  })));
  const view = mount(owner), old = view.debug();
  old.selectSource('https'); old.authorizeOperator('TEST-only'); old.setVisible(true);
  expect(requests).toHaveLength(1);
  view.render('OTHER-DEVICE');
  expect(requests[0].signal.aborted).toBe(true);
  expect(view.debug()).not.toBe(old);
  expect(old.getSnapshot()).toMatchObject({ operatorAuthorized: false, visible: false, events: [] });
  expect(view.debug().getSnapshot().operatorAuthorized).toBe(false);
  expect(view.panel().props.deviceId).toBe('OTHER-DEVICE');
  expect(view.panel().key).toBe('OTHER-DEVICE:canonical');
  requests[0].resolve(new Response(JSON.stringify({ device_id: 'TEST-DEVICE', events: [], next_after: '0', oldest_available: '0', retention_gap: false, has_more: false })));
  await Promise.resolve(); await Promise.resolve();
  expect(view.debug().getSnapshot().events).toEqual([]);
  expect(requests).toHaveLength(1);
  const current = view.debug(); current.selectSource('https'); current.authorizeOperator('TEST-second'); current.setVisible(true);
  expect(requests).toHaveLength(2);
  view.unmount();
  expect(requests[1].signal.aborted).toBe(true);
  expect(current.getSnapshot().operatorAuthorized).toBe(false);
  requests[1].resolve(new Response('{}'));
  await Promise.resolve(); await Promise.resolve();
  owner.dispose();
});

it('route does not hide its own admitted debug operation but reports a calibration lease as busy', async () => {
  const { owner, client } = ownerFixture(); await owner.connect();
  let finish!: (value: any) => void;
  client.debugMeta.mockReturnValue(new Promise(resolve => { finish = resolve; }));
  const view = mount(owner); view.debug().setVisible(true);
  expect(owner.getSnapshot().busy).toBe('debug');
  view.render(); expect(view.panel().props.busy).toBe(false);
  const release = owner.beginControlActivity('https-calibration');
  view.render(); expect(view.panel().props.busy).toBe(true);
  view.unmount();
  finish({ boot_id: '00000000000000000000000000000001', build_id: 'TEST', level: 'INFO' });
  await Promise.resolve(); await Promise.resolve();
  expect(client.debugPage).not.toHaveBeenCalled();
  release(); owner.dispose();
});
