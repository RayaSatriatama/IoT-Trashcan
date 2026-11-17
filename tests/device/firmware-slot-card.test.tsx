import React from 'react';
import { describe, expect, it, vi } from 'vitest';
const { renderToStaticMarkup } = require('react-dom/server') as { renderToStaticMarkup: (node: React.ReactNode) => string };
vi.mock('react-native', () => ({ Image: ({ accessibilityLabel }: { accessibilityLabel?: string }) => <img alt={accessibilityLabel} /> }));
import { TamaguiProvider } from 'tamagui';
import config from '../../tamagui.config';
import { FirmwareSlotCard } from '../../components/SlotCard';
import type { FirmwareResult } from '../../lib/api/types';

const result: FirmwareResult = {
  stream: 'organic', boot_id: 'private-boot', sequence: '9007199254740993', monotonic_us: '9007199254740993', recorded_at: '2026-11-12T10:00:00Z',
  time_quality: 'SYNCED', source_sample_ids: ['private-source'], result_origin: 'PHYSICAL', origin_firmware_version: 'private-fw', fresh: true, age_ms: 0,
  future_clock_skew: false, ordering_unknown: false,
  processed: { fill_pct: null, weight_kg: null, weight_utilization_pct: null, ogi: null, accumulation_hours: null, temperature_c: null },
  fuzzy: { state: 'READY', score: 0, label: null, reason: 'INPUT_MISSING', model_state: 'PRODUCTION', trace: null },
  safety: { state: 'CLEAR', reason: 'SAFETY_NOT_CONFIGURED', model_state: 'PRODUCTION', trace: null },
};
const render = (value: FirmwareResult) => renderToStaticMarkup(<TamaguiProvider config={config} defaultTheme="light"><FirmwareSlotCard result={value} /></TamaguiProvider>);

describe('FirmwareSlotCard original design', () => {
  it('uses the original card and retains a finite production zero score', () => {
    const html = render(result);
    for (const label of ['Slot organik', 'Keputusan angkut', 'Prioritas angkut', 'Dasar keputusan angkut', 'Sensor terkait:']) expect(html).toContain(label);
    expect(html).toContain('0 / 100');
    expect(html).not.toContain('FIS');
    expect(html).not.toContain('Hasil pemrosesan MCU');
  });

  it('keeps prototype scores nonoperational without adding UI or fabricating missing metrics', () => {
    const html = render({ ...result, processed: { fill_pct: 0, weight_kg: 2.5, weight_utilization_pct: null, ogi: 17, accumulation_hours: null, temperature_c: null }, fuzzy: { ...result.fuzzy, score: 88, label: 'Normal', model_state: 'TEST_ONLY' } });
    expect(html).not.toContain('88 / 100');
    expect(html).not.toMatch(/>Normal</);
    expect(html).toContain('2.5 kg');
    expect(html).toContain('Utilisasi berat');
    expect(html).toContain('aria-valuenow="2.5"');
    expect(html).toContain('aria-valuetext="2.5 kg / 2 kg"');
    expect(html).not.toContain('FIS');
  });

  it('shows temperature and caps the weight bar at 2 kg while hiding stale Normal model status', () => {
    const current = {
      ...result,
      processed: { fill_pct: 37, weight_kg: -0.18, weight_utilization_pct: null, ogi: null, accumulation_hours: null, temperature_c: 24.8 },
      fuzzy: { ...result.fuzzy, label: 'Normal' },
    };
    const html = render(current);
    expect(html).toContain('0.18 kg');
    expect(html).not.toContain('-0.18 kg');
    expect(html).toContain('24.8 °C');
    expect(html).not.toContain('Akumulasi');
    expect(html).toContain('aria-valuemax="2"');
    expect(html).toMatch(/>Normal</);

    const stale = render({ ...current, age_ms: 30_001 });
    expect(stale).not.toMatch(/>Normal</);
    const staleWarning = render({ ...current, age_ms: 30_001, fuzzy: { ...current.fuzzy, label: 'Waspada' } });
    expect(staleWarning).toMatch(/>Waspada</);
  });

  it.each([Number.NaN, Number.POSITIVE_INFINITY, Number.NEGATIVE_INFINITY])('omits non-finite production score %s', score => {
    expect(render({ ...result, fuzzy: { ...result.fuzzy, score } })).not.toContain('/ 100');
  });

  it.each(['UNCONFIGURED', 'UNAVAILABLE'] as const)('keeps unavailable model %s empty', state => {
    const html = render({ ...result, fuzzy: { ...result.fuzzy, state } });
    expect(html).not.toContain('/ 100');
    expect(html).not.toContain(state);
  });

  it('retains production safety alerts without treating prototype CLEAR as safe', () => {
    expect(render({ ...result, safety: { ...result.safety, state: 'ALERT' } })).toContain('Safety Alert');
    const prototype = render({ ...result, stream: 'b3', safety: { ...result.safety, model_state: 'TEST_ONLY' } });
    expect(prototype).not.toMatch(/>Aman</);
  });

  it('uses recorded time only when available and synchronized', () => {
    expect(render(result)).toContain('17:00');
    expect(render({ ...result, time_quality: 'UNSYNCED' })).not.toContain('17:00');
    expect(render({ ...result, recorded_at: null })).not.toContain('17:00');
  });

  it('never exposes boot, sequence, source or firmware identifiers', () => {
    const html = render(result);
    for (const opaque of ['private-boot', '9007199254740993', 'private-source', 'private-fw']) expect(html).not.toContain(opaque);
  });
});
