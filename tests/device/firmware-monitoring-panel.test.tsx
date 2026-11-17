import React from 'react';
import { describe, expect, it, vi } from 'vitest';
const { renderToStaticMarkup } = require('react-dom/server') as { renderToStaticMarkup: (node: React.ReactNode) => string };
vi.mock('react-native', () => ({ Image: ({ accessibilityLabel }: { accessibilityLabel?: string }) => <img alt={accessibilityLabel} /> }));
import { TamaguiProvider } from 'tamagui';
import config from '../../tamagui.config';
import { DeviceCard } from '../../components/DeviceCard';
import type { FirmwareMonitoringSample } from '../../lib/api/types';

const sample: FirmwareMonitoringSample = {
  sensor_id: 'mq135', stream: 'organic', boot_id: 'opaque-boot', last_sequence: '9007199254740993',
  quality: 'UNCALIBRATED', reason: 'INVALID_PROFILE', raw: { adc_counts: 0 }, converted_value: null,
  sample_origin: 'PHYSICAL', origin_firmware_version: null, fresh: true, age_ms: 0, ordering_unknown: false, future_clock_skew: false,
};

describe('home device card boundary', () => {
  it('uses the provided friendly placement and mounts no firmware raw-data panels', () => {
    const html = renderToStaticMarkup(<TamaguiProvider config={config} defaultTheme="light"><DeviceCard device={{
      id: 'device-id', location_id: 'location-id', device_code: '0123456789abcdef0123456789abcdef', placement_label: 'Real Device',
      online_state: null, last_seen: null, status: null, slots: [], firmware_monitoring: [sample],
    }} onPress={() => {}} /></TamaguiProvider>);
    expect(html).toContain('Real Device');
    expect(html).not.toContain('0123456789abcdef0123456789abcdef');
    expect(html).not.toContain('0 counts');
    expect(html).not.toContain('opaque-boot');
    expect(html).not.toContain('Telemetri sensor firmware');
    expect(html).not.toContain('Hasil firmware MCU');
  });
});
