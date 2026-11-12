import { describe, expect, it, vi } from 'vitest';

vi.mock('expo-router', () => ({
  Link: ({ children }: { children: unknown }) => children,
  useLocalSearchParams: () => ({ id: 'fixture' }),
  useRouter: () => ({ back: vi.fn() }),
}));

vi.mock('react-native', () => ({
  Image: () => null,
}));
vi.mock('../../components/device/DeviceDebugPanel', () => ({ DeviceDebugPanel: () => null }));

import {
  LOCATION_EMPTY_DEVICES_COPY,
  deviceDetailHref,
} from '../../app/locations/[id]';
import {
  DEVICE_STREAM_LABELS,
  DEVICE_UNAVAILABLE_COPY,
  getDeviceStatusLabel,
  DeviceDetailSessionView,
} from '../../app/devices/[id]';

describe('location detail route contracts', () => {
  it('encodes device links under the device detail route', () => {
    expect(deviceDetailHref('SIGAP unit/01')).toBe('/devices/SIGAP%20unit%2F01');
  });

  it('provides specific copy when a location has no devices', () => {
    expect(LOCATION_EMPTY_DEVICES_COPY.title).toMatch(/device/i);
    expect(LOCATION_EMPTY_DEVICES_COPY.description).toMatch(/data|terhubung/i);
  });
});

describe('device detail route contracts', () => {
  it('keeps the three canonical stream labels', () => {
    expect(DEVICE_STREAM_LABELS).toEqual({
      organic: 'Organik',
      inorganic: 'Anorganik',
      b3: 'B3',
    });
  });

  it('exposes explicit offline and invalid status labels', () => {
    expect(getDeviceStatusLabel('Offline')).toBe('Offline');
    expect(getDeviceStatusLabel('Data Invalid')).toBe('Data tidak valid');
  });

  it('uses explicit unavailable copy for nullable metrics', () => {
    expect(DEVICE_UNAVAILABLE_COPY).toBe('Belum tersedia');
  });

  it('exports the shared device session composition for the device route', () => {
    expect(DeviceDetailSessionView).toBeTypeOf('function');
  });
});
