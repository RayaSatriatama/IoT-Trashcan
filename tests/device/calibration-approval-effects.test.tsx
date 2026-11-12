import { afterEach, describe, expect, it, vi } from 'vitest';
import type { CalibrationPreview } from '../../lib/api/calibration';

const runtime = vi.hoisted(() => ({ frame: null as any }));
const changed = (a: readonly unknown[] | undefined, b: readonly unknown[]) => !a || a.length !== b.length || b.some((v, i) => !Object.is(v, a[i]));
vi.mock('react', async (load) => {
  const React = await load<any>();
  return { ...React,
    useState(initial: any) { const f = runtime.frame; const i = f.si++; if (!(i in f.state)) f.state[i] = typeof initial === 'function' ? initial() : initial; return [f.state[i], (v: any) => { const n = typeof v === 'function' ? v(f.state[i]) : v; if (!Object.is(n, f.state[i])) { f.state[i] = n; f.dirty = true; } }]; },
    useRef(initial: any) { const f = runtime.frame; return f.refs[f.ri++] ??= { current: initial }; },
    useEffect(setup: any, deps: any[]) { const f = runtime.frame; const i = f.ei++; const old = f.effects[i]; if (changed(old?.deps, deps)) f.effects[i] = { deps, setup, cleanup: old?.cleanup, changed: true }; },
    useSyncExternalStore: (_s: any, get: any) => get(),
  };
});
vi.mock('tamagui', () => { const node = (props: any) => ({ props }); return { Card: node, Input: node, Text: node, XStack: node, YStack: node }; });
vi.mock('../../components/AppButton', () => ({ AppButton: (props: any) => ({ props }) }));
vi.mock('../../components/SectionHeader', () => ({ DescriptionHint: () => null }));
vi.mock('../../components/device/DeviceWorkflowSteps', () => ({ DeviceWorkflowSteps: () => null }));
vi.mock('../../lib/ui/motionContext', () => ({ useReducedMotion: () => false }));

import { CalibrationWizard, previewAgreementKey } from '../../components/device/CalibrationWizard';

const proposal: CalibrationPreview = { sensor_id: 'mq7', proposal_hash: 'proposal-test-current', profile_version: '12', parameters: { adc_reference_mv: 3300 }, evidence_count: 4, evidence_hash: 'evidence-test-current', expires_at: '2030-01-01T00:01:00.000Z' };
const identity = vi.fn(async (id: string) => id);
const commit = vi.fn(async () => ({ status: 'OK', sensor: 'mq7', data: { state: 'COMMITTED' } }));
const previewCall = vi.fn(async () => ({ status: 'OK', sensor: 'mq7', data: { state: 'PREVIEW_READY', proposal_hash: 'proposal-replacement', profile_version: '13', evidence_count: '5', evidence_hash: 'evidence-replacement', expires_at: '2030-01-01T00:02:00.000Z' } }));
const cancel = vi.fn(async () => ({ status: 'OK', sensor: 'mq7', data: { state: 'CANCELLED' } }));
const evidence = vi.fn(async () => ({ status: 'OK', sensor: 'mq7', data: { q: 'UNCALIBRATED', r: 'TEST_ONLY', s: '1', fr: 'FRESH', raw: '1' } }));
const adapter: any = { verifyDeviceIdentity: identity, calibrationCapabilities: vi.fn(async (sensor: string) => ({ status: 'OK', sensor, data: { references: 'none', criteria: 'none', engineering: 'none' } })), calibrationEvidence: evidence, calibrationStart: vi.fn(), calibrationCriteria: vi.fn(), calibrationCapture: vi.fn(), calibrationValidate: vi.fn(), calibrationPreview: previewCall, calibrationCommit: commit, calibrationCancel: cancel };
const props: any = { deviceId: 'device-test', sensorId: 'mq7', diagnostic: null, api: null, bleClient: adapter, bleConnected: true, transport: 'ble' };
const find = (tree: any, predicate: (props: any) => boolean): any => {
  if (!tree || typeof tree !== 'object') return;
  if (tree.props && predicate(tree.props)) return tree;
  for (const value of Object.values(tree)) { const result = find(value, predicate); if (result) return result; }
};
const setup = async (overrides: Record<string, unknown> = {}) => {
  let currentProps = { ...props, ...overrides };
  const frame: any = { state: [], refs: [], effects: [], si: 0, ri: 0, ei: 0, dirty: false };
  const render = (updates: Record<string, unknown> = {}) => { currentProps = { ...currentProps, ...updates }; let limit = 20; do { if (!limit--) throw Error('effects did not settle'); frame.si = frame.ri = frame.ei = 0; frame.dirty = false; runtime.frame = frame; try { frame.tree = CalibrationWizard(currentProps); } finally { runtime.frame = null; } const pending = frame.effects.filter((x: any) => x.changed); pending.forEach((x: any) => { x.cleanup?.(); x.cleanup = undefined; }); pending.forEach((x: any) => { x.changed = false; x.cleanup = x.setup() || undefined; }); } while (frame.dirty); };
  const button = (label: string) => find(frame.tree, p => p.ariaLabel === label);
  // Deliberately invokes guarded callbacks even when rendered disabled.
  const click = (label: string) => { const node = button(label); if (!node) throw Error(`button not found: ${label}`); return node.props.onPress(); };
  render();
  // Test-only controlled hook driver seeds an otherwise admitted proposal; callbacks remain production closures.
  Object.assign(frame.state, { 0: true, 1: 'commit', 2: 'session-test', 8: proposal, 9: null, 12: { sensor_id: 'mq7', references: [], criteria: [], engineering_parameters: [] }, 16: 'PREVIEW_READY', 18: { sensor_id: 'mq7', quality: 'UNCALIBRATED', reason: 'TEST_ONLY', raw: { present: true, heater_ready: true }, converted_value: null, converted_unit: null, fresh: true, age_ms: 0, last_sequence: null, profile_present: false, profile_version: null, profile_hash: null, calibration_state: 'PREVIEW_READY', accepted_samples: 4, rejected_samples: 0 } });
  if (currentProps.transport === 'https') frame.state[17] = frame.state[18];
  render();
  await turns(); render();
  click('Setujui proposal kalibrasi'); render();
  vi.clearAllMocks();
  const unmount = () => { for (const effect of frame.effects) { effect.cleanup?.(); effect.cleanup = undefined; } };
  return { frame, render, click, button, unmount };
};
const turns = async () => { for (let i = 0; i < 30; i++) await Promise.resolve(); };
afterEach(() => { vi.restoreAllMocks(); vi.useRealTimers(); for (const mock of [identity, commit, previewCall, cancel, evidence]) mock.mockClear(); runtime.frame = null; });

describe('CalibrationWizard production approval callbacks and effects', () => {
  const clock = () => { vi.useFakeTimers(); vi.setSystemTime(new Date('2030-01-01T00:00:30.000Z')); };
  it('commits an explicitly approved current BLE proposal with exact identity', async () => {
    clock(); const ui = await setup();
    expect(ui.button('Commit profil').props.disabled).toBe(false);
    ui.click('Commit profil'); await turns(); ui.render();
    expect(commit).toHaveBeenCalledExactlyOnceWith('mq7', { expected_profile_hash: proposal.proposal_hash, expected_profile_version: '12' });
    expect(identity).toHaveBeenCalledTimes(1);
    for (const mutation of [adapter.calibrationStart, adapter.calibrationCriteria, adapter.calibrationCapture, adapter.calibrationValidate, previewCall, cancel]) expect(mutation).not.toHaveBeenCalled();
  });
  it.each(['ble', 'https'])('blocks %s commit at activation-time expiry without any transport call', async transport => {
    clock(); const submit = vi.fn(); const ui = await setup({ transport, ...(transport === 'https' ? { api: { submit } } : {}) });
    const retained = ui.button('Commit profil').props.onPress;
    vi.setSystemTime(new Date(proposal.expires_at)); retained(); await turns(); ui.render();
    expect(identity).not.toHaveBeenCalled(); expect(commit).not.toHaveBeenCalled(); expect(submit).not.toHaveBeenCalled();
    expect(find(ui.frame.tree, p => p.role === 'alert').props.children).toContain('Persetujuan proposal terbaru diperlukan');
  });
  it.each([{ deviceId: 'device-next' }, { sensorId: 'mq4' }])('clears proposal and consent after context replacement %j', async update => {
    clock(); const ui = await setup(); ui.render(update); await turns(); ui.render();
    expect(ui.frame.state[8]).toBeNull(); expect(ui.frame.state[9]).toBeNull();
    expect(ui.button('Commit profil')).toBeUndefined(); expect(commit).not.toHaveBeenCalled();
  });
  it('clears proposal and consent on actual disconnect effects', async () => {
    clock(); const ui = await setup(); ui.render({ bleConnected: false }); await turns(); ui.render();
    expect(ui.frame.state[8]).toBeNull(); expect(ui.frame.state[9]).toBeNull();
    expect(ui.button('Commit profil')).toBeUndefined(); expect(commit).not.toHaveBeenCalled();
  });
  it('clears proposal and consent after acknowledged BLE cancel', async () => {
    clock(); const ui = await setup(); ui.click('Batalkan kalibrasi'); await turns(); ui.render();
    expect(cancel).toHaveBeenCalledTimes(1); expect(ui.frame.state[16]).toBe('CANCELLED');
    expect(ui.frame.state[8]).toBeNull(); expect(ui.frame.state[9]).toBeNull();
    expect(ui.button('Commit profil')).toBeUndefined(); expect(commit).not.toHaveBeenCalled();
  });
  it('replacement preview clears consent and its actual callback requires new explicit approval', async () => {
    clock(); const ui = await setup(); ui.frame.state[1] = 'preview'; ui.render();
    ui.click('Buat preview'); await turns(); ui.render();
    expect(previewCall).toHaveBeenCalledTimes(1); expect(ui.frame.state[9]).toBeNull();
    expect(ui.button('Commit profil').props.disabled).toBe(true); vi.clearAllMocks();
    ui.click('Commit profil'); await turns(); ui.render();
    expect(identity).not.toHaveBeenCalled(); expect(commit).not.toHaveBeenCalled();
    ui.click('Setujui proposal kalibrasi'); ui.render(); ui.click('Commit profil'); await turns(); ui.render();
    expect(commit).toHaveBeenCalledExactlyOnceWith('mq7', { expected_profile_hash: 'proposal-replacement', expected_profile_version: '13' });
  });
  it('keeps calibration inputs at the minimum touch target size', async () => {
    clock(); const ui = await setup();
    ui.frame.state[12] = { sensor_id: 'mq7', references: [{ name: 'primary', unit: 'mV', required: true }], criteria: [], engineering_parameters: [] }; ui.render();
    expect(find(ui.frame.tree, p => p['aria-label'] === 'Referensi primary').props.minHeight).toBeGreaterThanOrEqual(44);
  });
  it('masks both web HTTPS credential inputs while retaining native masking', async () => {
    clock(); const ui = await setup({ transport: 'https', bleClient: null });
    for (const label of ['Token sesi operator', 'Token CSRF sesi operator']) {
      const input = find(ui.frame.tree, p => p['aria-label'] === label);
      expect(input.props.type).toBe('password');
      expect(input.props.secureTextEntry).toBe(true);
    }
  });

  it.each([
    { deviceId: 'device-next' }, { sensorId: 'mq4' },
    { transport: 'https' }, { bleConnected: false },
  ])('ignores delayed BLE preview success and follow-up reads after context replacement %j', async update => {
    clock(); const refresh = vi.fn(); const ui = await setup({ onDiagnosticRefresh: refresh });
    let finish!: (value: any) => void;
    previewCall.mockImplementationOnce(() => new Promise(resolve => { finish = resolve; }));
    ui.frame.state[1] = 'preview'; ui.render(); ui.click('Buat preview'); await turns();
    expect(previewCall).toHaveBeenCalledTimes(1);
    ui.render(update); await turns(); ui.render(); evidence.mockClear();
    finish({ status: 'OK', sensor: 'mq7', sessionId: 'old-session', data: { state: 'PREVIEW_READY', proposal_hash: 'stale-proposal', profile_version: '13', evidence_count: '5', evidence_hash: 'stale-evidence', expires_at: proposal.expires_at } });
    await turns(); ui.render();
    expect(ui.frame.state[8]).toBeNull(); expect(ui.frame.state[2]).toBeNull();
    expect(ui.frame.state[1]).toBe('diagnostics'); expect(ui.frame.state[4]).toBeNull();
    expect(evidence).not.toHaveBeenCalled(); expect(refresh).not.toHaveBeenCalled();
  });

  it('does not admit a BLE mutation after delayed identity verification crosses a context boundary', async () => {
    clock(); const ui = await setup(); let finish!: (value: string) => void;
    identity.mockImplementationOnce(() => new Promise(resolve => { finish = resolve; }));
    ui.frame.state[1] = 'preview'; ui.render(); ui.click('Buat preview'); await turns();
    ui.render({ sensorId: 'mq4' }); await turns(); finish('device-test'); await turns(); ui.render();
    expect(previewCall).not.toHaveBeenCalled(); expect(ui.frame.state[8]).toBeNull();
  });

  it('does not repopulate a replacement context with a delayed BLE error', async () => {
    clock(); const ui = await setup(); let fail!: (cause: Error) => void;
    previewCall.mockImplementationOnce(() => new Promise((_resolve, reject) => { fail = reject; }));
    ui.frame.state[1] = 'preview'; ui.render(); ui.click('Buat preview'); await turns();
    ui.render({ sensorId: 'mq4' }); await turns(); fail(new Error('stale operation failed')); await turns(); ui.render();
    expect(ui.frame.state[4]).toBeNull(); expect(ui.frame.state[8]).toBeNull();
  });

  it('does not continue HTTPS polling after unmount and holds the operation lease until the request settles', async () => {
    clock(); let finish!: (value: any) => void;
    const getCommand = vi.fn((_device: string, _id: string, _signal?: AbortSignal) => new Promise(resolve => { finish = resolve; }));
    const api = { submit: vi.fn(async () => ({ command: { command_id: 'old-command', device_id: 'device-test', sensor_id: 'mq7', operation: 'PREVIEW', status: 'QUEUED', expires_at: proposal.expires_at } })), getCommand };
    const releases: ReturnType<typeof vi.fn>[] = [];
    const activity = vi.fn(() => { const release = vi.fn(); releases.push(release); return release; });
    const refresh = vi.fn(); const ui = await setup({ transport: 'https', api, onControlActivity: activity, onDiagnosticRefresh: refresh });
    ui.frame.state[1] = 'preview'; ui.render(); ui.click('Buat preview'); await turns(); ui.render();
    expect(getCommand).toHaveBeenCalledTimes(1); const signal = getCommand.mock.calls[0]?.[2] as AbortSignal | undefined;
    ui.unmount(); expect(signal?.aborted).toBe(true);
    expect(releases.some(release => release.mock.calls.length === 0)).toBe(true);
    finish({ command: { command_id: 'old-command', operation: 'COMMIT', status: 'COMMITTED', expires_at: proposal.expires_at } });
    await turns(); await vi.advanceTimersByTimeAsync(20000);
    expect(getCommand).toHaveBeenCalledTimes(1); expect(refresh).not.toHaveBeenCalled();
    expect(releases.every(release => release.mock.calls.length === 1)).toBe(true);
  });

  it.each(['submit', 'poll'] as const)('discards late HTTPS %s results and persistence after context replacement', async phase => {
    clock(); let finish!: (value: any) => void;
    const queued = { command_id: 'old-command', device_id: 'device-test', sensor_id: 'mq7', operation: 'PREVIEW', status: 'QUEUED', expires_at: proposal.expires_at };
    const pending = () => new Promise<any>(resolve => { finish = resolve; });
    const api = { submit: vi.fn(phase === 'submit' ? pending : async () => ({ command: queued })), getCommand: vi.fn(pending) };
    const refresh = vi.fn(); const ui = await setup({ transport: 'https', api, onDiagnosticRefresh: refresh });
    ui.frame.state[1] = 'preview'; ui.render(); ui.click('Buat preview'); await turns();
    expect(api.submit).toHaveBeenCalledTimes(1);
    expect(api.getCommand).toHaveBeenCalledTimes(phase === 'poll' ? 1 : 0);
    ui.render({ deviceId: 'device-next', sensorId: 'mq4' }); await turns(); ui.render();
    finish({ command: { ...queued, status: 'COMMITTED' }, session_id: 'old-session', preview: proposal });
    await turns(); await vi.advanceTimersByTimeAsync(20000); ui.render();
    expect(ui.frame.state[8]).toBeNull(); expect(ui.frame.state[2]).toBeNull();
    expect(ui.frame.state[14]).toBeNull(); expect(ui.frame.state[1]).toBe('diagnostics');
    expect(ui.frame.state[4]).toBeNull(); expect(refresh).not.toHaveBeenCalled();
    expect(api.getCommand).toHaveBeenCalledTimes(phase === 'poll' ? 1 : 0);
  });

  it('cancels the HTTPS poll delay before admitting a follow-up request', async () => {
    clock(); const queued = { command_id: 'old-command', device_id: 'device-test', sensor_id: 'mq7', operation: 'PREVIEW', status: 'QUEUED', expires_at: proposal.expires_at };
    const api = { submit: vi.fn(async () => ({ command: queued })), getCommand: vi.fn(async () => ({ command: queued })) };
    const ui = await setup({ transport: 'https', api }); ui.frame.state[1] = 'preview'; ui.render();
    ui.click('Buat preview'); await turns(); expect(api.getCommand).toHaveBeenCalledTimes(1);
    ui.render({ sensorId: 'mq4' }); await turns(); await vi.advanceTimersByTimeAsync(20000); ui.render();
    expect(api.getCommand).toHaveBeenCalledTimes(1); expect(ui.frame.state[3]).toBe(false);
  });

  it('rejects an old rendered handler invoked after a context replacement', async () => {
    clock(); const ui = await setup(); ui.frame.state[1] = 'preview'; ui.render();
    const retained = ui.button('Buat preview').props.onPress;
    ui.render({ sensorId: 'mq4' }); await turns(); ui.render(); vi.clearAllMocks();
    retained(); await turns(); ui.render();
    expect(identity).not.toHaveBeenCalled(); expect(previewCall).not.toHaveBeenCalled();
    expect(ui.frame.state[8]).toBeNull();
  });
});
