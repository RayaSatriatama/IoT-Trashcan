import { afterEach, describe, expect, it, vi } from 'vitest';

const runtime = vi.hoisted(() => ({ frame: null as any }));
const motion = vi.hoisted(() => ({ reduced: false }));
const changed = (a: readonly unknown[] | undefined, b: readonly unknown[]) => !a || a.length !== b.length || b.some((v, i) => !Object.is(v, a[i]));
vi.mock('react', async load => {
  const React = await load<any>();
  return { ...React,
    useState(initial: any) { const f = runtime.frame; const i = f.si++; if (!(i in f.state)) f.state[i] = typeof initial === 'function' ? initial() : initial; return [f.state[i], (v: any) => { const n = typeof v === 'function' ? v(f.state[i]) : v; if (!Object.is(n, f.state[i])) { f.state[i] = n; f.dirty = true; } }]; },
    useRef(initial: any) { const f = runtime.frame; return f.refs[f.ri++] ??= { current: initial }; },
    useMemo(fn: any) { return fn(); },
    useEffect(setup: any, deps: any[]) { const f = runtime.frame; const i = f.ei++; const old = f.effects[i]; if (changed(old?.deps, deps)) f.effects[i] = { deps, setup, cleanup: old?.cleanup, changed: true }; },
  };
});
vi.mock('tamagui', () => { const node = (props: any) => ({ props }); return { Card: node, Text: node, XStack: node, YStack: node }; });
vi.mock('../../components/SensorStateBadge', () => ({ SensorStateBadge: (props: any) => ({ props }) }));
vi.mock('../../lib/ui/motionContext', () => ({ useReducedMotion: () => motion.reduced }));
import { LiveSensorDashboard } from '../../components/device/LiveSensorDashboard';
import type { FirmwareMonitoringSample } from '../../lib/api/types';

const sample = (overrides: Partial<FirmwareMonitoringSample> = {}): FirmwareMonitoringSample => ({
  sensor_id: 'sht31_44', stream: 'organic', boot_id: 'boot-a', last_sequence: '14750',
  quality: 'READY', reason: 'DATA_READY', sample_origin: 'UNKNOWN', origin_firmware_version: null,
  raw: { temperature_ticks: 0, humidity_ticks: 0 }, converted_value: { temperature_c: 32.5, humidity_percent: 68.02 },
  fresh: null, age_ms: 2_786_083, ordering_unknown: true, future_clock_skew: false, ...overrides,
});
const recent = (overrides: Partial<FirmwareMonitoringSample> = {}) => sample({ last_sequence: '14799', age_ms: 1_500, ordering_unknown: false, ...overrides });

const setup = (monitoring: FirmwareMonitoringSample[]) => {
  const f: any = { state: [], refs: [], effects: [], si: 0, ri: 0, ei: 0, dirty: false };
  const render = (next: FirmwareMonitoringSample[]) => {
    let limit = 20;
    do {
      if (!limit--) throw Error('effects did not settle');
      f.si = f.ri = f.ei = 0;
      f.dirty = false;
      runtime.frame = f;
      try { f.tree = LiveSensorDashboard({ monitoring: next }); } finally { runtime.frame = null; }
      const pending = f.effects.filter((x: any) => x.changed);
      pending.forEach((x: any) => { x.cleanup?.(); x.cleanup = undefined; });
      pending.forEach((x: any) => { x.changed = false; x.cleanup = x.setup() || undefined; });
    } while (f.dirty);
    f.text = JSON.stringify(f.tree);
    return f;
  };
  render(monitoring);
  return { f, render, unmount: () => f.effects.forEach((x: any) => { x.cleanup?.(); x.cleanup = undefined; }) };
};
const find = (tree: any, predicate: (props: any) => boolean): any => {
  if (!tree || typeof tree !== 'object') return undefined;
  if (tree.props && predicate(tree.props)) return tree;
  for (const value of Object.values(tree)) { const found = find(value, predicate); if (found) return found; }
  return undefined;
};
afterEach(() => { runtime.frame = null; motion.reduced = false; vi.useRealTimers(); vi.restoreAllMocks(); });

describe('live sensor dashboard', () => {
  it('does not present a library-default curve as calibrated ppm',()=>{
    const ui=setup([recent({sensor_id:'mq135',converted_value:{adc_mv:2500},raw:{adc_counts:2000}})]);
    expect(ui.f.text).toContain('Tegangan AO');
    expect(ui.f.text).toContain('2500 mV');
    expect(ui.f.text).toContain('Belum terkalibrasi');
    expect(ui.f.text).not.toContain('Estimasi CO₂ ekuivalen');
    expect(ui.f.text).not.toContain('32782 ppm');
    expect(find(ui.f.tree,p=>p.state==='VALID')).toBeUndefined();
    ui.unmount();
  });
  it('declares that it is a polled snapshot rather than a real-time stream', () => {
    const ui = setup([sample()]);
    expect(ui.f.text).toContain('Polling otomatis');
    expect(ui.f.text).toContain('bukan kebijakan freshness server');
    expect(ui.f.text).not.toContain('real-time');
    ui.unmount();
  });

  it('shows the unset freshness policy, observed age, and ordering uncertainty per sensor', () => {
    const ui = setup([sample()]);
    expect(ui.f.text).toContain('Kesegaran belum ditetapkan API');
    expect(ui.f.text).toContain('Umur sampel (estimasi): 46 menit');
    expect(ui.f.text).toContain('Urutan lintas boot tidak diketahui');
    expect(ui.f.text).toContain('Asal tidak diketahui');
    ui.unmount();
  });

  it('renders converted engineering values, keeping a real zero and never raw counts', () => {
    const ui = setup([sample({ converted_value: { temperature_c: 0, humidity_percent: 0 } })]);
    expect(ui.f.text).toContain('0 °C');
    expect(ui.f.text).toContain('0 %RH');
    expect(ui.f.text).not.toContain('counts');
    ui.unmount();
  });

  it('does not animate or announce anything on the first observation or an unchanged poll', () => {
    const ui = setup([sample()]);
    expect(ui.f.text).not.toContain('Contoh baru teramati');
    expect(ui.f.text).not.toContain('"animation":"data"');
    ui.render([sample()]);
    expect(ui.f.text).not.toContain('Contoh baru teramati');
    expect(ui.f.text).not.toContain('"animation":"data"');
    ui.unmount();
  });

  it('announces and animates only a new recent capture, then stops after its timeout', () => {
    vi.useFakeTimers();
    const ui = setup([sample()]);
    ui.render([recent()]);
    expect(ui.f.text).toContain('Sampel baru diterima');
    expect(ui.f.text).toContain('"transition":"data"');
    expect(find(ui.f.tree, p => p['data-arrival'] === 'true')).toBeDefined();
    vi.advanceTimersByTime(1200);
    ui.render([recent()]);
    expect(ui.f.text).not.toContain('Sampel baru diterima');
    expect(find(ui.f.tree, p => p['data-arrival'] === 'true')).toBeUndefined();
    ui.unmount();
  });

  it('refuses to call a late capture a live arrival', () => {
    const ui = setup([sample()]);
    ui.render([sample({ last_sequence: '14800' })]);
    expect(ui.f.text).not.toContain('Contoh baru teramati');
    expect(ui.f.text).not.toContain('"animation":"data"');
    ui.unmount();
  });

  it('keeps a finite arrival pulse across age-only ticks and clears it by timeout', () => {
    vi.useFakeTimers();
    const ui = setup([sample()]); ui.render([recent()]);
    ui.render([recent({ age_ms: 2000 })]);
    expect(find(ui.f.tree, p => p['data-arrival'] === 'true')).toBeDefined();
    vi.advanceTimersByTime(1200); ui.render([recent({ age_ms: 3200 })]);
    expect(find(ui.f.tree, p => p['data-arrival'] === 'true')).toBeUndefined();
    ui.unmount(); vi.useRealTimers();
  });

  it('keeps reduced motion free of data-arrival animation while preserving the announcement', () => {
    motion.reduced = true;
    const ui = setup([sample()]);
    ui.render([recent()]);
    expect(ui.f.text).toContain('Sampel baru diterima');
    expect(find(ui.f.tree, p => p['data-arrival'] === 'true')).toBeUndefined();
    expect(find(ui.f.tree, p => p.transition !== undefined)).toBeUndefined();
    expect(find(ui.f.tree, p => p.enterStyle !== undefined)).toBeUndefined();
    ui.unmount();
  });

  it('never schedules a network poller and only bounds the visual pulse with a timeout', () => {
    const interval = vi.spyOn(globalThis, 'setInterval');
    const timeout = vi.spyOn(globalThis, 'setTimeout');
    const ui = setup([sample()]);
    ui.render([recent()]);
    expect(interval).not.toHaveBeenCalled();
    expect(timeout).toHaveBeenCalledTimes(1);
    expect(timeout).toHaveBeenCalledWith(expect.any(Function), 1200);
    interval.mockRestore();
    timeout.mockRestore();
    ui.unmount();
  });

  it('shows one explicit unavailable state when the API has no samples', () => {
    const ui = setup([]);
    expect(ui.f.text).toContain('Monitoring sensor live belum tersedia pada API');
    expect(ui.f.text).not.toContain('Kesegaran');
    ui.unmount();
  });
});
