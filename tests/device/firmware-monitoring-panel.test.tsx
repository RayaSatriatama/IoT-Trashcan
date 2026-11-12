import React from 'react';
const { renderToStaticMarkup } = require('react-dom/server') as { renderToStaticMarkup: (node: React.ReactNode) => string };
import { describe, expect, it, vi } from 'vitest';
vi.mock('react-native', () => ({ Image: ({ accessibilityLabel }: { accessibilityLabel?: string }) => <img alt={accessibilityLabel} /> }));
import { TamaguiProvider } from 'tamagui';
import config from '../../tamagui.config';
import { FirmwareMonitoringPanel } from '../../components/device/FirmwareMonitoringPanel';
import { DeviceCard } from '../../components/DeviceCard';
import type { FirmwareMonitoringSample } from '../../lib/api/types';

const sample = (extra: Partial<FirmwareMonitoringSample> = {}): FirmwareMonitoringSample => ({
  sensor_id: 'mq135', stream: 'ORGANIC', boot_id: 'boot-test', last_sequence: '9007199254740993',
  quality: 'UNCALIBRATED', reason: 'PROFILE_MISSING', raw: { adc_counts: 0 }, converted_value: null,
  sample_origin: 'PHYSICAL', origin_firmware_version: 'sigap-task6-physical4-v2',
  age_ms: null, fresh: null, ordering_unknown: true, future_clock_skew: false, ...extra,
});
const render = (samples: FirmwareMonitoringSample[] | undefined, compact = false) => renderToStaticMarkup(
  <TamaguiProvider config={config} defaultTheme="light"><FirmwareMonitoringPanel samples={samples} compact={compact} /></TamaguiProvider>,
);

describe('public archive monitoring render boundary', () => {
  it('reaches the actual shared home DeviceCard when legacy slots are empty', () => {
    const html = renderToStaticMarkup(<TamaguiProvider config={config} defaultTheme="light"><DeviceCard device={{
      id: 'test-device', location_id: 'test-location', device_code: 'TEST-ONLY', placement_label: null,
      online_state: null, last_seen: null, status: null, slots: [], firmware_monitoring: [sample()],
    }} onPress={() => {}} /></TamaguiProvider>);
    expect(html).toContain('0 counts');
    expect(html).toContain('UNCALIBRATED');
    expect(html).not.toContain('Belum ada data slot');
    expect(html).not.toMatch(/<button/);
  });
  it('shows raw zero before calibration without operator authorization', () => {
    const html = render([sample()]);
    expect(html).toContain('0 counts');
    expect(html).toContain('UNCALIBRATED');
    expect(html).toContain('PROFILE_MISSING');
    expect(html).toContain('Fisik (metadata firmware)');
    expect(html).not.toContain('Otorisasi operator');
    expect(html).not.toContain('ppm');
    expect(html).not.toContain('boot-test');
    expect(html).not.toContain('9007199254740993');
  });
  it('identifies only absent channels from the exact physical4 profile metadata', () => {
    const html = render([sample()]);
    expect(html).toContain('SHT31');
    expect(html).toContain('Belum ada sampel untuk kanal pada profil firmware ini.');
    expect(html).toContain('metadata firmware');
    expect(html).not.toMatch(/SHT31[\\s\\S]{0,120}(TIMEOUT|0 °C|0%)/);
    const withSht = render([sample(), sample({sensor_id: 'sht31_44'})]);
    expect(withSht).toContain('SHT31 0x44');
    expect(withSht.split('Belum ada sampel untuk kanal pada profil firmware ini.').length - 1).toBe(2);
    expect(render([sample({sample_origin: 'SIMULATED'})])).not.toContain('Belum ada sampel untuk kanal');
    expect(render([sample({sample_origin: 'UNKNOWN'})])).not.toContain('Belum ada sampel untuk kanal');
    expect(render([sample({origin_firmware_version: null})])).not.toContain('Belum ada sampel untuk kanal');
    expect(render([sample({origin_firmware_version: 'sigap-task6-physical4-v2-spoof'})])).not.toContain('Belum ada sampel untuk kanal');
    expect(render([sample(), sample({origin_firmware_version: 'sigap-task6-physical4-v1'})])).not.toContain('Belum ada sampel untuk kanal');
  });
  it('keeps simulated and unknown origins distinct from physical metadata', () => {
    expect(render([sample({sample_origin: 'SIMULATED'})])).toContain('Simulasi sensor firmware');
    expect(render([sample({sample_origin: 'UNKNOWN'})])).toContain('Asal tidak diketahui');
    expect(render([sample({sample_origin: 'SIMULATED'})])).not.toContain('Fisik (metadata firmware)');
  });
  it('retains timing uncertainty and does not promote raw readings to engineering values', () => {
    const html = render([sample({converted_value: {adc_mv: 1200}})]);
    expect(html).toContain('Umur tidak diketahui');
    expect(html).toContain('Urutan lintas boot tidak diketahui');
    expect(html).not.toContain('1200 mV');
  });
  it('shows stale and future-clock conditions without calling samples fresh', () => {
    expect(render([sample({quality: 'STALE', age_ms: 1200, fresh: false})])).toContain('Kualitas tidak segar');
    expect(render([sample({future_clock_skew: true})])).toContain('Waktu arsip di masa depan');
  });
  it('keeps empty archive and missing API projection explicit', () => {
    expect(render([])).toContain('Belum ada sampel firmware yang diterima');
    expect(render(undefined)).toContain('Monitoring firmware belum tersedia pada API');
  });
  it('renders compact preview without nested interactive controls', () => {
    const html = render([sample()], true);
    expect(html).toContain('0 counts');
    expect(html).not.toMatch(/<button/);
    expect(html).not.toContain('Identitas ROM');
  });
});
