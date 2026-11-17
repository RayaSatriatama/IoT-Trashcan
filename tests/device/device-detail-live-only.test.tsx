import React from 'react';
import { describe, expect, it, vi } from 'vitest';
const { renderToStaticMarkup } = require('react-dom/server') as { renderToStaticMarkup: (node: React.ReactNode) => string };
vi.mock('react-native', () => ({ Image: ({ accessibilityLabel }: { accessibilityLabel?: string }) => <img alt={accessibilityLabel} /> }));
vi.mock('react-native-svg', () => ({ Circle: () => null, Svg: () => null }));
import { TamaguiProvider } from 'tamagui';
import config from '../../tamagui.config';
import { DeviceDecisionSummary } from '../../components/DeviceDecisionSummary';
import { DeviceSlotCards } from '../../components/SlotCard';
import type { FirmwareMonitoringSample } from '../../lib/api/types';

// Test-only fixture; these values are not live device data.
const monitoring: FirmwareMonitoringSample[] = [
  { sensor_id: 'hx711_organic', stream: 'organic', boot_id: 'fixture', last_sequence: '1', quality: 'READY', reason: 'DATA_READY', sample_origin: 'UNKNOWN', origin_firmware_version: null, raw: null, converted_value: { kg: -0.18 }, fresh: null, age_ms: 1500, ordering_unknown: true, future_clock_skew: false },
  { sensor_id: 'ultrasonic_organic', stream: 'organic', boot_id: 'fixture', last_sequence: '2', quality: 'READY', reason: 'DATA_READY', sample_origin: 'UNKNOWN', origin_firmware_version: null, raw: null, converted_value: { distance_mm: 120, fill_percent: 36 }, fresh: null, age_ms: 1800, ordering_unknown: true, future_clock_skew: false },
  { sensor_id: 'mq135', stream: 'organic', boot_id: 'fixture', last_sequence: '3', quality: 'READY', reason: 'DATA_READY', sample_origin: 'UNKNOWN', origin_firmware_version: null, raw: null, converted_value: { adc_mv: 2800 }, fresh: null, age_ms: 2100, ordering_unknown: true, future_clock_skew: false },
  { sensor_id: 'sht31_44', stream: 'organic', boot_id: 'fixture', last_sequence: '4', quality: 'READY', reason: 'DATA_READY', sample_origin: 'UNKNOWN', origin_firmware_version: null, raw: null, converted_value: { temperature_c: 24.8, humidity_percent: 50 }, fresh: null, age_ms: 2200, ordering_unknown: true, future_clock_skew: false },
];
const render = (node: React.ReactNode) => renderToStaticMarkup(<TamaguiProvider config={config} defaultTheme="light">{node}</TamaguiProvider>);

describe('device detail live-only decision UI', () => {
  it('uses the original organic card labels without additional monitoring UI or invented values', () => {
    const html = render(<DeviceSlotCards firmwareResults={[]} slots={[]} monitoring={monitoring} deviceId="fixture-device" cardWidth="100%" />);
    expect(html).toContain('Slot Organik');
    expect(html).toContain('36%');
    expect(html).toContain('0.18 kg');
    expect(html).not.toContain('-0.18 kg');
    for (const label of ['Slot organik', 'Gas', 'Suhu', 'Update', 'Dasar keputusan angkut', 'Lima kondisi pembentuk prioritas pengangkutan.', 'Utilisasi berat', 'Indeks gas', 'Waktu akumulasi', 'Sensor terkait:']) expect(html).toContain(label);
    expect(html).toContain('24.8 °C');
    expect(html).toContain('SHT31');
    expect(html).not.toContain('MQ-4');
    // The declared organic capacity is now 2 kg.
    expect(html).toContain('aria-valuemax="2"');
    expect(html).toContain('aria-valuenow="0.18"');
    // 0.18 kg against the 2 kg capacity is 9%.
    expect(html).toContain('9%');
    expect(html).not.toContain('Status akhir');
    for (const added of ['2800 mV', 'Fuzzy/FIS', 'Sensor live', 'Gas ADC', 'Pembacaan sensor', 'Kesegaran', 'Asal tidak diketahui']) expect(html).not.toContain(added);
    expect(html).not.toContain('/ 100');
    expect(html).toMatch(/>Normal</);
  });

  it('removes the receiving-data Normal badge if a required organic sensor is stale, invalid, future-dated, missing, or rejected by API freshness', () => {
    const changed = (sensorId: string, patch: Partial<FirmwareMonitoringSample>) => monitoring.map(item => item.sensor_id === sensorId ? { ...item, ...patch } : item);
    const samplesWithoutSht31 = monitoring.filter(item => item.sensor_id !== 'sht31_44');
    const cases = [
      changed('sht31_44', { age_ms: 30_001 }),
      changed('sht31_44', { fresh: false }),
      changed('sht31_44', { future_clock_skew: true }),
      changed('sht31_44', { quality: 'INVALID' }),
      samplesWithoutSht31,
    ];
    for (const samples of cases) {
      const html = render(<DeviceSlotCards firmwareResults={[]} slots={[]} monitoring={samples} deviceId="fixture-device" cardWidth="100%" />);
      expect(html).not.toMatch(/>Normal</);
    }
  });

  it('keeps only the original concise decision-summary fields', () => {
    const html = render(<DeviceDecisionSummary summary={null} firmwareResults={[]} />);
    expect(html).toContain('Ringkasan keputusan');
    for (const label of ['Prioritas angkut', 'Slot paling kritis', 'Mode keputusan']) expect(html).toContain(label);
    expect(html).not.toContain('FIS');
    expect(html).not.toContain('Fuzzy:');
    expect(html).toContain('0 / 100');
  });
});
