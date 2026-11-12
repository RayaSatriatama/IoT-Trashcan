import { readFileSync } from 'node:fs';
import { afterEach, expect, it, vi } from 'vitest';
import { createDebugApi } from '../../lib/api/debug';
import { DebugViewController } from '../../lib/debug/poller';
import { decodeDebugResponse } from '../../lib/debug/codec';
import { DeviceBleSession } from '../../lib/ble/device-session';
import { WifiSettingsController } from '../../lib/ble/wifi-settings';

const recorded = JSON.parse(readFileSync(new URL('../fixtures/debug-production-history.json', import.meta.url), 'utf8'));
const wire = Object.fromEntries(readFileSync(new URL('../../contracts/debug/v1/fixtures/ble.txt', import.meta.url), 'utf8').trimEnd().split('\n').map(line => {
  const at = line.indexOf('='); return [line.slice(0, at), line.slice(at + 1)];
}));
function setup() {
  const meta = decodeDebugResponse(wire.META), page = decodeDebugResponse(wire.PAGE);
  if (meta.kind !== 'meta' || page.kind !== 'page') throw Error('actual C++ fixture kinds');
  const client: any = { connect: vi.fn(async () => {}), close: vi.fn(),
    verifyDeviceIdentity: vi.fn(async (id: string) => id),
    debugMeta: vi.fn(async () => meta.meta), debugPage: vi.fn(async () => page.page),
    status: vi.fn(async () => ({ state: 'CONNECTED', ssid: null })) };
  const owner = new DeviceBleSession('canonical-TEST', { createClient: () => client });
  return { owner, client };
}
afterEach(() => vi.restoreAllMocks());

it.each(['history', 'empty_history', 'boot_change_history', 'retention_history'])('actual Go %s crosses API decoding into the real view controller', async key => {
  const { owner } = setup();
  const fetcher = vi.fn(async () => new Response(JSON.stringify(recorded[key]), { status: 200 }));
  const api = createDebugApi({ bearerToken: 'TEST-operator' }, fetcher, 'http://TEST.invalid');
  const view = new DebugViewController({ deviceId: recorded.device_id, session: owner, history: api.history });
  view.selectSource('https'); view.setVisible(true);
  expect(fetcher).not.toHaveBeenCalled();
  view.authorizeOperator('TEST-operator');
  await vi.waitFor(() => expect(view.getSnapshot().history).not.toBeNull());
  expect(recorded.provenance).toBe('TEST');
  const snapshot = view.getSnapshot();
  expect(snapshot.error).toBeNull();
  expect(snapshot.events.map(item => item.event?.sequence)).toEqual(recorded[key].events.map((item: any) => item.sequence));
  expect(snapshot.after).toBe(recorded[key].next_after);
  expect(snapshot.history?.retention_gap).toBe(recorded[key].retention_gap);
  expect(fetcher).toHaveBeenCalledWith(expect.stringContaining('/api/device-debug/events/TEST-DEVICE?after=0'), expect.objectContaining({ method: 'GET', signal: expect.any(AbortSignal) }));
  if (key === 'boot_change_history') expect(new Set(snapshot.events.map(item => item.bootId)).size).toBe(2);
  if (snapshot.events.length) expect(snapshot.events[0].capturedMonotonicUs).toBe('9007199254740993');
  view.dispose(); owner.dispose();
});

it('uses actual decoded C++ BLE records and shared Wi-Fi/calibration admission without retries', async () => {
  const { owner, client } = setup(); await owner.connect();
  const wifi = new WifiSettingsController(owner);
  const view = new DebugViewController({ deviceId: recorded.device_id, session: owner });
  const release = owner.beginControlActivity('https-calibration');
  view.setVisible(true); await Promise.resolve();
  expect(client.debugMeta).not.toHaveBeenCalled();
  release();
  await vi.waitFor(() => expect(view.getSnapshot().events).toHaveLength(1));
  expect(view.getSnapshot().meta?.build_id).toBe('TEST');
  expect(view.getSnapshot().events[0].event?.monotonic_us).toBe('9007199254740993');
  const count = client.debugMeta.mock.calls.length;
  let finish!: (value: any) => void;
  client.status.mockReturnValueOnce(new Promise(resolve => { finish = resolve; }));
  const operation = wifi.loadStatus();
  await view.refresh(); expect(client.debugMeta).toHaveBeenCalledTimes(count);
  finish({ state: 'CONNECTED', ssid: null }); await operation;
  await vi.waitFor(() => expect(client.debugMeta).toHaveBeenCalledTimes(count + 1));
  expect(client.status).toHaveBeenCalledTimes(1);
  view.dispose(); wifi.dispose(); owner.dispose();
});

it('keeps the real expired-operator response unauthorized instead of displaying stale history', async () => {
  const { owner } = setup();
  const api = createDebugApi({ bearerToken: 'TEST-expired' }, async () => new Response(JSON.stringify(recorded.expired_operator_body), { status: recorded.expired_operator_status }), 'http://TEST.invalid');
  const view = new DebugViewController({ deviceId: recorded.device_id, session: owner, history: api.history });
  view.selectSource('https'); view.authorizeOperator('TEST-expired'); view.setVisible(true);
  await vi.waitFor(() => expect(view.getSnapshot().error).toMatch(/unauthorized/i));
  expect(view.getSnapshot().events).toEqual([]);
  view.dispose(); owner.dispose();
});
