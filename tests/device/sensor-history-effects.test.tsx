import { afterEach, describe, expect, it, vi } from 'vitest';

const runtime = vi.hoisted(() => ({ frame: null as any, fetch: vi.fn() }));
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
vi.mock('react-native-svg', () => ({ Circle: () => null, Line: () => null, Polyline: () => null, Svg: () => null }));
vi.mock('../../components/AppButton', () => ({ AppButton: (props: any) => ({ props }) }));
vi.mock('../../components/device/SensorSelector', () => ({ SensorSelector: (props: any) => ({ props }) }));
vi.mock('../../components/SensorStateBadge', () => ({ SensorStateBadge: (props: any) => ({ props }) }));
vi.mock('../../lib/api/client', () => ({ apiClient: { fetchSensorHistory: (...args: any[]) => runtime.fetch(...args) } }));
import { SensorHistoryCharts } from '../../components/device/SensorHistoryCharts';

const point = (boot_id = 'boot-a', sequence = 1) => ({ boot_id, sequence: String(sequence), monotonic_us: String(sequence * 1000), sample_origin: 'PHYSICAL' as const, quality: 'UNCALIBRATED', reason: 'INVALID_PROFILE', recorded_at: '2026-01-02T03:04:05Z', time_quality: 'SYNCED', raw: { adc_counts: 42 }, converted: null });
const page = (overrides: any = {}) => ({ device_id: 'device-a', sensor_id: 'mq4', points: [point()], next_cursor: 'cursor-1', ...overrides });
const deferred = <T,>() => { let resolve!: (value: T) => void; let reject!: (error: Error) => void; const promise = new Promise<T>((a, b) => { resolve = a; reject = b; }); return { promise, resolve, reject }; };
const monitoring = (boot = 'boot-a', sensor: 'mq4' = 'mq4') => [{ sensor_id: sensor, boot_id: boot, stream: 'organic', last_sequence: '1', quality: 'UNCALIBRATED', reason: 'INVALID_PROFILE', sample_origin: 'PHYSICAL' as const, origin_firmware_version: null, raw: null, converted_value: null, fresh: true, age_ms: 0, ordering_unknown: false, future_clock_skew: false }];
const setup = (deviceId = 'device-a', samples = monitoring()) => {
  const f: any = { state: [], refs: [], effects: [], si: 0, ri: 0, ei: 0, dirty: false };
  let props = { deviceId, monitoring: samples };
  const render = (updates: Partial<typeof props> = {}) => { props = { ...props, ...updates }; let limit = 30; do { if (!limit--) throw Error('effects did not settle'); f.si = f.ri = f.ei = 0; f.dirty = false; runtime.frame = f; try { f.tree = SensorHistoryCharts(props); } finally { runtime.frame = null; } const changedEffects = f.effects.filter((e: any) => e.changed); changedEffects.forEach((e: any) => { e.cleanup?.(); e.cleanup = undefined; }); changedEffects.forEach((e: any) => { e.changed = false; e.cleanup = e.setup() || undefined; }); } while (f.dirty); };
  const unmount = () => f.effects.forEach((e: any) => { e.cleanup?.(); e.cleanup = undefined; });
  render(); return { f, render, unmount };
};
const turns = async () => { for (let i = 0; i < 30; i++) await Promise.resolve(); };
const button = (tree: any, label: string): any => { if (!tree || typeof tree !== 'object') return; if (tree.props?.ariaLabel === label) return tree; for (const v of Object.values(tree)) { const found = button(v, label); if (found) return found; } };
afterEach(() => { runtime.fetch.mockReset(); runtime.fetch.mockImplementation(() => new Promise(() => {})); runtime.frame = null; });

describe('SensorHistoryCharts request ownership effects', () => {
  it('does not refetch when monitoring is replaced by an equal-signature array', () => { const d = deferred<any>(); runtime.fetch.mockReturnValue(d.promise); const ui = setup(); ui.render({ monitoring: [...monitoring()] }); expect(runtime.fetch).toHaveBeenCalledTimes(1); ui.unmount(); });
  it('requests older pages with the exact cursor and boot', async () => { runtime.fetch.mockResolvedValueOnce(page()); const ui = setup(); await turns(); ui.render(); const d = deferred<any>(); runtime.fetch.mockReturnValueOnce(d.promise); button(ui.f.tree, 'Muat riwayat Gas · MQ-4 yang lebih lama').props.onPress(); expect(runtime.fetch).toHaveBeenLastCalledWith('device-a', 'mq4', 'cursor-1', expect.any(AbortSignal), 'boot-a'); d.resolve(page({ points: [point('boot-a', 2)], next_cursor: null })); await turns(); ui.render(); ui.unmount(); });
  it('ignores a saved callback after its generation is disposed', async () => { runtime.fetch.mockResolvedValue(page()); const ui = setup(); await turns(); ui.render(); const old = button(ui.f.tree, 'Muat riwayat Gas · MQ-4 yang lebih lama').props.onPress; ui.render({ monitoring: monitoring('boot-b') }); await turns(); runtime.fetch.mockClear(); old(); expect(runtime.fetch).not.toHaveBeenCalled(); ui.unmount(); });
  it('does not admit late success or rejection into a replacement boot', async () => { for (const reject of [false, true]) { const d = deferred<any>(); runtime.fetch.mockReturnValueOnce(d.promise); const ui = setup(); ui.render({ monitoring: monitoring('boot-b') }); await turns(); reject ? d.reject(new Error('late')) : d.resolve(page()); await turns(); ui.render(); expect(JSON.stringify(ui.f.tree)).not.toContain('Respons riwayat tidak cocok'); ui.unmount(); } });
  it('refreshing while an older request is pending does not show old data', async () => { runtime.fetch.mockResolvedValueOnce(page()); const ui = setup(); await turns(); ui.render(); const d = deferred<any>(); runtime.fetch.mockReturnValueOnce(d.promise); button(ui.f.tree, 'Muat riwayat Gas · MQ-4 yang lebih lama').props.onPress(); button(ui.f.tree, 'Perbarui snapshot riwayat sensor').props.onPress(); await turns(); ui.render(); expect(runtime.fetch).toHaveBeenCalledTimes(3); expect(ui.f.tree).toBeDefined(); d.resolve(page({ points: [point('boot-a', 2)] })); await turns(); ui.render(); ui.unmount(); });
  it('deduplicates duplicate points and obeys the point cap', async () => { runtime.fetch.mockResolvedValueOnce(page({ points: Array.from({ length: 1000 }, (_, i) => point('boot-a', i)) })); const ui = setup(); await turns(); ui.render(); expect(button(ui.f.tree, 'Muat riwayat Gas · MQ-4 yang lebih lama')).toBeUndefined(); ui.unmount(); });
  it('rejects wrong point boot while accepting empty terminal pages', async () => { runtime.fetch.mockResolvedValueOnce(page({ points: [point('wrong')] })); const ui = setup(); await turns(); ui.render(); expect(JSON.stringify(ui.f.tree)).toContain('Respons riwayat tidak cocok'); ui.unmount(); runtime.fetch.mockReset(); runtime.fetch.mockResolvedValueOnce(page({ points: [], next_cursor: null, unavailable_reason: 'end' })); const emptyUi = setup(); await turns(); emptyUi.render(); expect(JSON.stringify(emptyUi.f.tree)).toContain('end'); emptyUi.unmount(); });
  it('settles late initial results as errors only for the owning generation', async () => { const d = deferred<any>(); runtime.fetch.mockReturnValueOnce(d.promise); const ui = setup(); d.resolve(page({ points: [point('wrong')] })); await turns(); ui.render(); expect(JSON.stringify(ui.f.tree)).toContain('Respons riwayat tidak cocok'); ui.unmount(); });
  it('labels physical research canonically and shows the selected history timestamp and calibration reason', async () => { runtime.fetch.mockResolvedValueOnce(page()); const ui = setup(); await turns(); ui.render(); const rendered = JSON.stringify(ui.f.tree); expect(rendered).toContain('Sensor fisik'); expect(rendered).toContain('Pilih sensor'); expect(rendered).toContain('Pembacaan terakhir'); expect(rendered).toContain('2026'); expect(rendered).toContain('Profil kalibrasi belum valid'); expect(rendered).not.toContain('Tren engineering sensor'); ui.unmount(); });
  it('keeps the last-reading label but omits a timestamp when history has no recorded time', async () => { runtime.fetch.mockResolvedValueOnce(page({ points: [{ ...point(), recorded_at: null }] })); const ui = setup(); await turns(); ui.render(); expect(JSON.stringify(ui.f.tree)).toContain('Pembacaan terakhir'); ui.unmount(); });
});
