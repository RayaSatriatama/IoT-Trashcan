import React from 'react';
import { describe, expect, it, vi } from 'vitest';
const { renderToStaticMarkup } = require('react-dom/server') as { renderToStaticMarkup: (node: React.ReactNode) => string };
vi.mock('react-native-svg', () => ({ Circle: () => null, Line: () => null, Polyline: () => null, Svg: () => null }));
vi.mock('react-native', () => ({ Image: ({ accessibilityLabel }: { accessibilityLabel?: string }) => <img alt={accessibilityLabel} /> }));
vi.mock('../../components/device/SensorSelector', () => ({
  SensorSelector: () => <span>Pilih sensor</span>,
}));
import { TamaguiProvider } from 'tamagui';
import config from '../../tamagui.config';
import { ResearchPanel } from '../../components/ResearchPanel';
import { DeviceCard } from '../../components/DeviceCard';
import type { FirmwareMonitoringSample, FirmwareResult } from '../../lib/api/types';

const monitoring: FirmwareMonitoringSample[] = [{
  sensor_id: 'mq135', stream: 'organic', boot_id: 'opaque-boot-id', last_sequence: '9007199254740993',
  quality: 'UNCALIBRATED', reason: 'INVALID_PROFILE', sample_origin: 'PHYSICAL', origin_firmware_version: null,
  raw: { adc_counts: 42 }, converted_value: null, fresh: true, age_ms: 0, ordering_unknown: false, future_clock_skew: false,
}];
const result: FirmwareResult = {
  stream: 'organic', boot_id: 'opaque-result-boot', sequence: '9007199254740993', monotonic_us: '9007199254740993',
  recorded_at: '2026-11-12T10:00:00Z', time_quality: 'SYNCED', source_sample_ids: ['opaque-source-id'], result_origin: 'PHYSICAL',
  origin_firmware_version: null, fresh: true, age_ms: 0, future_clock_skew: false, ordering_unknown: false,
  processed: { fill_pct: null, weight_kg: null, weight_utilization_pct: null, ogi: null, accumulation_hours: null, temperature_c: null },
  fuzzy: { state: 'UNCONFIGURED', score: null, label: null, reason: 'MODEL_NOT_CONFIGURED', model_state: 'UNCONFIGURED', trace: null },
  safety: { state: 'UNCONFIGURED', reason: 'SAFETY_NOT_CONFIGURED', model_state: 'UNCONFIGURED', trace: null },
};
const render = (node: React.ReactNode) => renderToStaticMarkup(<TamaguiProvider config={config} defaultTheme="light">{node}</TamaguiProvider>);

describe('research composition', () => {
  it('keeps MCU results and sensor trends in one Detail Riset and hides opaque identifiers/raw values', () => {
    const html = render(<ResearchPanel data={null} firmwareResults={[result]} deviceId="route-id" monitoring={monitoring} />);
    expect(html.match(/Detail Riset/g)).toHaveLength(1);
    expect(html).toContain('Hasil pemrosesan MCU');
    expect(html).toContain('Sensor fisik');
    expect(html).toContain('MQ-135');
    expect(html).toContain('Nilai engineering tepercaya belum tersedia');
    expect(html).not.toContain('42 counts');
    for (const opaque of ['opaque-boot-id', 'opaque-result-boot', 'opaque-source-id', '9007199254740993']) expect(html).not.toContain(opaque);
  });

  it('keeps unavailable converted values unavailable and presents DeviceCard with the friendly placement label', () => {
    const html = render(<>
      <ResearchPanel data={null} firmwareResults={[result]} deviceId="route-id" monitoring={monitoring} />
      <DeviceCard device={{ id: 'route-id', location_id: 'location-id', device_code: '0123456789abcdef0123456789abcdef', placement_label: 'Real Device', online_state: null, last_seen: null, status: null, slots: [], firmware_monitoring: monitoring, firmware_results: [result] }} />
    </>);
    expect(html).toContain('Real Device');
    expect(html).toContain('Belum tersedia');
    expect(html).not.toContain('0123456789abcdef0123456789abcdef');
    expect(html).not.toContain('Sensor firmware');
    expect(html).not.toContain('Hasil firmware MCU');
  });
});
