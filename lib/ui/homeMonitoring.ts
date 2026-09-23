import type { CanonicalStatus, DeviceDetail, DeviceSummary, LocationDetail } from '../api/types';

export type HomeMonitoringMode = 'locations' | 'devices' | 'attention';
export type HomeOperationalFilter = 'Angkat' | 'Safety Alert' | null;

export interface HomeDeviceEntry {
  device: DeviceDetail;
  location: LocationDetail;
}

export interface HomeAffectedLocation {
  location: LocationDetail;
  devices: DeviceDetail[];
}

export function flattenHomeDevices(locations: LocationDetail[]): HomeDeviceEntry[] {
  return locations.flatMap((location) => location.devices.map((device) => ({ device, location })));
}

export function groupDeviceSummaries(devices: DeviceSummary[]): LocationDetail[] {
  const groups = new Map<string, LocationDetail>();
  for (const device of devices) {
    const existing = groups.get(device.location_id);
    if (existing) {
      existing.devices.push(device);
      continue;
    }
    groups.set(device.location_id, {
      id: device.location_id,
      name: device.location_name,
      address_text: device.location_address_text,
      status: device.location_status,
      devices: [device],
    });
  }
  return [...groups.values()];
}

export function getAffectedLocations(
  locations: LocationDetail[],
  status: Exclude<HomeOperationalFilter, null>,
): HomeAffectedLocation[] {
  return locations
    .map((location) => ({
      location,
      devices: location.devices.filter((device) => device.status === status),
    }))
    .filter((entry) => entry.devices.length > 0);
}

export function getOperationalFilterLabel(status: Exclude<HomeOperationalFilter, null>): string {
  return status === 'Angkat' ? 'Perlu diangkat' : 'Peringatan keselamatan';
}

export function matchesOperationalFilter(device: DeviceDetail, status: CanonicalStatus): boolean {
  return device.status === status;
}
