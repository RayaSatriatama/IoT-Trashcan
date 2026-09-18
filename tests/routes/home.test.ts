import { describe, expect, it, vi } from 'vitest';

vi.mock('expo-router', () => ({
  Link: ({ children }: { children: unknown }) => children,
  useRouter: () => ({ push: vi.fn() }),
}));

vi.mock('react-native', () => ({
  Image: () => null,
}));

import {
  HOME_KPI_LABELS,
  PRIORITY_EMPTY_COPY,
  RECENT_ACTIVITY_LIMIT,
  recentActivityItems,
} from '../../app/index';
import { flattenHomeDevices, getAffectedLocations } from '../../lib/ui/homeMonitoring';
import type { LocationDetail } from '../../lib/api/types';

const fixtureLocations: LocationDetail[] = [
  {
    id: 'loc-a',
    name: 'Lokasi A',
    address_text: 'Area A',
    status: 'Safety Alert',
    devices: [
      { id: 'dev-a', location_id: 'loc-a', device_code: 'SIM-A', placement_label: 'Lantai 1', online_state: 'Online', last_seen: null, status: 'Safety Alert', slots: [] },
      { id: 'dev-b', location_id: 'loc-a', device_code: 'SIM-B', placement_label: 'Lantai 2', online_state: 'Online', last_seen: null, status: 'Angkat', slots: [] },
    ],
  },
  {
    id: 'loc-b',
    name: 'Lokasi B',
    address_text: 'Area B',
    status: 'Normal',
    devices: [
      { id: 'dev-c', location_id: 'loc-b', device_code: 'SIM-C', placement_label: 'Lobi', online_state: 'Online', last_seen: null, status: 'Normal', slots: [] },
    ],
  },
];

describe('home route display contracts', () => {
  it('keeps the overview to four KPI labels', () => {
    expect(HOME_KPI_LABELS).toHaveLength(4);
    expect(new Set(HOME_KPI_LABELS).size).toBe(4);
  });

  it('caps recent activity at three items', () => {
    expect(RECENT_ACTIVITY_LIMIT).toBe(3);
    expect(recentActivityItems([1, 2, 3, 4])).toEqual([1, 2, 3]);
  });

  it('provides a specific empty priority copy', () => {
    expect(PRIORITY_EMPTY_COPY.title).toMatch(/prioritas/i);
    expect(PRIORITY_EMPTY_COPY.description).toBeTruthy();
  });

  it('flattens all devices while preserving location context', () => {
    expect(flattenHomeDevices(fixtureLocations).map(({ device, location }) => `${device.id}:${location.name}`)).toEqual([
      'dev-a:Lokasi A',
      'dev-b:Lokasi A',
      'dev-c:Lokasi B',
    ]);
  });

  it('groups only devices matching the selected operational filter', () => {
    expect(getAffectedLocations(fixtureLocations, 'Angkat')).toEqual([
      { location: fixtureLocations[0], devices: [fixtureLocations[0].devices[1]] },
    ]);
    expect(getAffectedLocations(fixtureLocations, 'Safety Alert')).toEqual([
      { location: fixtureLocations[0], devices: [fixtureLocations[0].devices[0]] },
    ]);
  });
});
