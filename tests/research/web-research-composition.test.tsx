import React from 'react';
import { describe, expect, it, vi } from 'vitest';
const { renderToStaticMarkup } = require('react-dom/server') as { renderToStaticMarkup: (node: React.ReactNode) => string };
vi.mock('react-native-svg', () => ({ Circle: () => null, Line: () => null, Polyline: () => null, Svg: () => null }));
vi.mock('react-native', () => ({ Image: ({ accessibilityLabel }: { accessibilityLabel?: string }) => <img alt={accessibilityLabel} /> }));
// Chart effects/select rendering have their own tests; isolate the parent composition boundary.
vi.mock('../../components/device/SensorHistoryCharts', () => ({ SensorHistoryCharts: () => <div>Sensor history boundary</div> }));
import { TamaguiProvider } from 'tamagui';
import config from '../../tamagui.config';
import { ResearchPanel } from '../../components/ResearchPanel';
import type { FirmwareMonitoringSample, FirmwareResult } from '../../lib/api/types';

// Unit-test fixtures only. These are not device observations or live evidence.
const monitoring: FirmwareMonitoringSample[] = [{
  sensor_id: 'hx711_organic', stream: 'organic', boot_id: 'private-fixture-boot',
  last_sequence: '9007199254740993', quality: 'READY', reason: 'DATA_READY',
  sample_origin: 'UNKNOWN', origin_firmware_version: null, raw: null,
  converted_value: { kg: 0 }, fresh: null, age_ms: 0,
  ordering_unknown: true, future_clock_skew: false,
}];
const result: FirmwareResult = {
  stream: 'organic', boot_id: 'private-fixture-mcu-boot', sequence: '9007199254740993',
  monotonic_us: '9007199254740993', recorded_at: '2026-11-12T10:00:00Z',
  time_quality: 'SYNCED', source_sample_ids: ['private-fixture-source'],
  result_origin: 'UNKNOWN', origin_firmware_version: null, fresh: false,
  age_ms: 36_000_000, future_clock_skew: false, ordering_unknown: true,
  processed: { fill_pct: null, weight_kg: null, weight_utilization_pct: null,
    ogi: null, accumulation_hours: null, temperature_c: null },
  fuzzy: { state: 'UNCONFIGURED', score: null, label: null,
    reason: 'MODEL_NOT_CONFIGURED', model_state: 'UNCONFIGURED', trace: null },
  safety: { state: 'UNCONFIGURED', reason: 'SAFETY_NOT_CONFIGURED',
    model_state: 'UNCONFIGURED', trace: null },
};
const render = (node: React.ReactNode) => renderToStaticMarkup(
  <TamaguiProvider config={config} defaultTheme="light">{node}</TamaguiProvider>,
);

describe('web research integration', () => {
  it('renders the TEST_ONLY web analysis independently of MCU results', () => {
    const html = render(<ResearchPanel deviceId="fixture-device" monitoring={monitoring} firmwareResults={[]} />);
    expect(html).toContain('TEST_ONLY');
    expect(html).toContain('Analisis web');
    expect(html).not.toContain('Data riset belum tersedia');
    expect(html).toContain('Hasil MCU belum tersedia');
  });

  it('separates historical MCU output from web-derived analysis', () => {
    const html = render(<ResearchPanel deviceId="fixture-device" monitoring={monitoring} firmwareResults={[result]} />);
    expect(html).toContain('Hasil MCU terakhir');
    expect(html).toContain('TEST_ONLY');
    expect(html.match(/Detail Riset/g)).toHaveLength(1);
    for (const opaque of ['private-fixture-boot', 'private-fixture-mcu-boot', 'private-fixture-source', '9007199254740993']) {
      expect(html).not.toContain(opaque);
    }
  });

  it('does not attach a device-specific web model to an unscoped legacy panel', () => {
    const html = render(<ResearchPanel data={null} />);
    expect(html).not.toContain('TEST_ONLY');
  });
});
