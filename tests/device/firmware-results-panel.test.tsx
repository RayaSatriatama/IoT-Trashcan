import React from 'react';
const { renderToStaticMarkup } = require('react-dom/server') as { renderToStaticMarkup: (node: React.ReactNode) => string };
import { describe, expect, it, vi } from 'vitest';
vi.mock('react-native', () => ({ Image: ({ accessibilityLabel }: { accessibilityLabel?: string }) => <img alt={accessibilityLabel} /> }));
import { TamaguiProvider } from 'tamagui';
import config from '../../tamagui.config';
import { FirmwareResultsPanel } from '../../components/device/FirmwareResultsPanel';
import { DeviceCard } from '../../components/DeviceCard';
import type { FirmwareResult } from '../../lib/api/types';

const canonicalTrace: FirmwareResult['fuzzy']['trace'] = {
  model_ref: { version: '7', hash: `sha256:${'a'.repeat(64)}` },
  memberships: [{ input: 'fill_pct', set: 'low', value: 0 }, { input: 'fill_pct', set: 'high', value: 1 }],
  rules: [{ id: 'r0', strength: 0, output_id: 'normal' }],
  aggregate: { output_id: 'normal', area: 0, centroid: null },
};
const readyResult: FirmwareResult = {
  stream: 'organic', boot_id: 'boot', sequence: '9007199254740993', monotonic_us: '9007199254740993',
  recorded_at: '2026-11-12T10:00:00Z', time_quality: 'SYNCED', source_sample_ids: ['s1'],
  result_origin: 'PHYSICAL', origin_firmware_version: 'fw-v3', fresh: true, age_ms: 0,
  future_clock_skew: false, ordering_unknown: false,
  processed: { fill_pct: 0, weight_kg: null, weight_utilization_pct: 0, ogi: 0, accumulation_hours: null, temperature_c: 0 },
  fuzzy: { state: 'READY', score: 0, label: 'normal', reason: 'computed', model_state: 'TEST_ONLY', trace: canonicalTrace },
  safety: { state: 'CLEAR', reason: 'clear', model_state: 'TEST_ONLY', trace: { ...canonicalTrace, aggregate: null } },
};
const unconfigured: FirmwareResult = { ...readyResult,
  fuzzy: { state: 'UNCONFIGURED', score: null, label: null, reason: 'model not configured', model_state: 'UNCONFIGURED', trace: null },
  safety: { state: 'UNCONFIGURED', reason: 'model not configured', model_state: 'UNCONFIGURED', trace: null },
};
const render = (node: React.ReactNode) => renderToStaticMarkup(<TamaguiProvider config={config} defaultTheme="light">{node}</TamaguiProvider>);

describe('firmware result rendered visibility', () => {
  it('renders canonical v3 trace fields including exact identifiers, zeroes, and null aggregates', () => {
    const html = render(<FirmwareResultsPanel results={[readyResult]} />);
    for (const text of ['Keterisian: 0%', 'Berat: Tidak tersedia', 'Utilisasi berat: 0%', 'OGI: 0', 'Waktu akumulasi: Tidak tersedia', 'Suhu: 0 °C', '9007199254740993', 'Fisik (metadata firmware) · firmware fw-v3', 'TEST_ONLY (bukan produksi)', 'model 7', 'sha256:', 'fill_pct · low: 0', 'Aturan r0: kekuatan 0 · normal', 'Agregat normal: area 0 · centroid tidak tersedia']) expect(html).toContain(text);
    expect(html).not.toContain('probabilitas'); expect(html).not.toContain('triggered_rules');
  });
  it('marks null traces unavailable and keeps unconfigured status/reason explicit', () => {
    const html = render(<FirmwareResultsPanel results={[unconfigured]} />);
    expect(html).toContain('FIS: UNCONFIGURED'); expect(html).toContain('model not configured'); expect(html).toContain('Trace FIS: trace tidak tersedia');
  });
  it('keeps fuzzy/safety TEST_ONLY warning visible in actual compact DeviceCard composition', () => {
    const html = render(<DeviceCard device={{ id:'d', location_id:'l', device_code:'unit', placement_label:null, online_state:'Online', last_seen:null, status:null, slots:[], firmware_results:[readyResult] }} onPress={() => {}} />);
    expect(html).toContain('FIS: READY'); expect(html).toContain('Safety: CLEAR'); expect(html.match(/TEST_ONLY \(bukan produksi\)/g)?.length).toBe(2); expect(html).not.toMatch(/<button/);
  });
  it('distinguishes absent API results from an accepted empty result list', () => {
    expect(render(<FirmwareResultsPanel />)).toContain('Hasil firmware belum tersedia pada API');
    expect(render(<FirmwareResultsPanel results={[]} />)).toContain('Belum ada hasil pemrosesan firmware');
  });
  it('renders simulated and unknown origin without claiming attestation', () => {
    for (const [origin, expected] of [['SIMULATED', 'Simulasi'], ['UNKNOWN', 'Tidak diketahui']] as const)
      expect(render(<FirmwareResultsPanel results={[{ ...readyResult, result_origin: origin, origin_firmware_version: null }]} />)).toContain(expected);
  });
  it('does not render unknown processed fields and accepts empty canonical trace arrays', () => {
    const noDetails: FirmwareResult = {
      ...readyResult,
      processed: { ...readyResult.processed, undocumented_metric: 123456789 } as FirmwareResult['processed'],
      fuzzy: { ...readyResult.fuzzy, trace: { ...canonicalTrace, memberships: [], rules: [], aggregate: null } },
    };
    const html = render(<FirmwareResultsPanel results={[noDetails]} />);
    expect(html).toContain('Trace FIS: model 7'); expect(html).toContain('Agregat: tidak tersedia');
    expect(html).not.toContain('undocumented_metric'); expect(html).not.toContain('123456789');
  });
  it('shows valid synced future clock separately from unknown unsynced time', () => {
    const future = { ...readyResult, recorded_at: '2099-01-01T00:00:00Z', future_clock_skew: true, age_ms: null, fresh: null };
    const unknown = { ...readyResult, recorded_at: null, time_quality: 'UNSYNCED' as const, future_clock_skew: false, age_ms: null, fresh: null };
    expect(render(<FirmwareResultsPanel results={[future]} />)).toContain('jam firmware di masa depan');
    const html = render(<FirmwareResultsPanel results={[unknown]} />);
    expect(html).toContain('waktu tidak tersinkron'); expect(html).toContain('umur tidak diketahui'); expect(html).not.toContain('jam firmware di masa depan');
  });
});
