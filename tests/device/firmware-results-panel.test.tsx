import React from 'react';
import { describe, expect, it, vi } from 'vitest';
const { renderToStaticMarkup } = require('react-dom/server') as { renderToStaticMarkup: (node: React.ReactNode) => string };
vi.mock('react-native', () => ({ Image: ({ accessibilityLabel }: { accessibilityLabel?: string }) => <img alt={accessibilityLabel} /> }));
import { TamaguiProvider } from 'tamagui';
import config from '../../tamagui.config';
import { DeviceDecisionSummary } from '../../components/DeviceDecisionSummary';
import { FirmwareResearch } from '../../components/device/FirmwareResearch';
import type { FirmwareResult } from '../../lib/api/types';

const result: FirmwareResult = {
  stream: 'organic', boot_id: 'private-boot', sequence: '9007199254740993', monotonic_us: '9007199254740993', recorded_at: '2026-11-12T10:00:00Z',
  time_quality: 'SYNCED', source_sample_ids: ['private-source'], result_origin: 'PHYSICAL', origin_firmware_version: 'private-fw', fresh: true, age_ms: 0,
  future_clock_skew: false, ordering_unknown: false,
  processed: { fill_pct: null, weight_kg: null, weight_utilization_pct: null, ogi: null, accumulation_hours: null, temperature_c: null },
  fuzzy: { state: 'UNCONFIGURED', score: null, label: null, reason: 'MODEL_NOT_CONFIGURED', model_state: 'UNCONFIGURED', trace: null },
  safety: { state: 'UNCONFIGURED', reason: 'SAFETY_NOT_CONFIGURED', model_state: 'UNCONFIGURED', trace: null },
};
const render = (node: React.ReactNode) => renderToStaticMarkup(<TamaguiProvider config={config} defaultTheme="light">{node}</TamaguiProvider>);

describe('MCU research presentation', () => {
  it('shows only canonical stream names, human statuses, actual timestamps, and unavailable null metrics', () => {
    const html = render(<FirmwareResearch results={[result]} />);
    expect(html).toContain('Organik');
    expect(html).toContain('Belum dikonfigurasi');
    expect(html).toContain('Model keselamatan belum dikonfigurasi');
    expect(html).toContain('Belum tersedia');
    expect(html).not.toContain('private-boot');
    expect(html).not.toContain('private-source');
    expect(html).not.toContain('private-fw');
    expect(html).not.toContain('MODEL_NOT_CONFIGURED');
    expect(html).not.toContain('sequence');
  });

  it('keeps decision safety and priority closed when MCU models are unconfigured', () => {
    const html = render(<DeviceDecisionSummary summary={null} firmwareResults={[{ ...result, safety: { ...result.safety, state: 'CLEAR' } }]} />);
    expect(html).toContain('Belum dinilai');
    expect(html).not.toContain('Aman');
    expect(html).not.toContain('UNCONFIGURED');
  });
});
