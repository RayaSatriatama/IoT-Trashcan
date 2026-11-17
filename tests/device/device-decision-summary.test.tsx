import React from 'react';
import { describe, expect, it } from 'vitest';
const { renderToStaticMarkup } = require('react-dom/server') as { renderToStaticMarkup: (node: React.ReactNode) => string };
import { TamaguiProvider } from 'tamagui';
import config from '../../tamagui.config';
import { DeviceDecisionSummary } from '../../components/DeviceDecisionSummary';
import { ReducedMotionContext } from '../../lib/ui/motionContext';
import type { DeviceDecisionSummaryDTO, FirmwareResult } from '../../lib/api/types';

const result = (stream: FirmwareResult['stream'], score: number): FirmwareResult => ({
  stream, boot_id: 'fixture', sequence: '1', monotonic_us: '1', recorded_at: null, time_quality: 'UNSYNCED',
  source_sample_ids: [], result_origin: 'UNKNOWN', origin_firmware_version: null, fresh: null, age_ms: null,
  future_clock_skew: false, ordering_unknown: false,
  processed: { fill_pct: null, weight_kg: null, weight_utilization_pct: null, ogi: null, accumulation_hours: null, temperature_c: null },
  fuzzy: { state: 'READY', score, label: null, reason: 'computed', model_state: 'PRODUCTION', trace: null },
  safety: { state: 'UNCONFIGURED', reason: 'SAFETY_NOT_CONFIGURED', model_state: 'UNCONFIGURED', trace: null },
});
const renderSummary = (summary: DeviceDecisionSummaryDTO | null, firmwareResults: FirmwareResult[] = []) =>
  renderToStaticMarkup(
    <ReducedMotionContext.Provider value>
      <TamaguiProvider config={config} defaultTheme="light">
        <DeviceDecisionSummary summary={summary} firmwareResults={firmwareResults} />
      </TamaguiProvider>
    </ReducedMotionContext.Provider>,
  );

const decisionSummary = (priority_score: number | null): DeviceDecisionSummaryDTO => ({
  priority_score, critical_stream: 'inorganic', critical_reason: null, mode: null,
});

describe('decision summary header', () => {
  it('uses a marked numeric initial value and fixed labels when no valid score exists', () => {
    const html = renderSummary(null);
    expect(html).toContain('0 / 100');
    expect(html).toContain('decision-summary-priority-initial');
    expect(html).toContain('Organik');
    expect(html).toContain('Penilaian multi-parameter');
    expect(html).not.toContain('Belum tersedia dari API');
  });

  it('keeps an actual API zero as a score rather than treating it as missing', () => {
    const html = renderSummary(decisionSummary(0));
    expect(html).toContain('0 / 100');
    expect(html).not.toContain('decision-summary-priority-initial');
    expect(html).toContain('Organik');
    expect(html).toContain('Penilaian multi-parameter');
  });

  it('uses only the actual organic MCU score and keeps zero valid', () => {
    for (const [organicScore, expected] of [[0, '0 / 100'], [37.6, '38 / 100']] as const) {
      const html = renderSummary(null, [result('organic', organicScore), result('b3', 99)]);
      expect(html).toContain(expected);
      expect(html).toContain('Organik');
      expect(html).not.toContain('B3');
      expect(html).not.toContain('decision-summary-priority-initial');
    }
  });

  it('uses initial zero for out-of-range or non-finite API and organic MCU scores', () => {
    for (const score of [Number.NaN, -1, 101]) {
      const apiHtml = renderSummary(decisionSummary(score));
      expect(apiHtml).toContain('0 / 100');
      expect(apiHtml).toContain('decision-summary-priority-initial');

      const mcuHtml = renderSummary(null, [result('organic', score), result('inorganic', 80)]);
      expect(mcuHtml).toContain('0 / 100');
      expect(mcuHtml).toContain('decision-summary-priority-initial');
      expect(mcuHtml).toContain('Organik');
      expect(mcuHtml).not.toContain('80 / 100');
    }
  });
});
