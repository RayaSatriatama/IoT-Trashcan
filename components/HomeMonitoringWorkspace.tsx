import type { LocationDetail } from '../lib/api/types';
import { getAffectedLocations, getOperationalFilterLabel, flattenHomeDevices, type HomeMonitoringMode, type HomeOperationalFilter } from '../lib/ui/homeMonitoring';
import { paginate } from '../lib/ui/pagination';
import { AppButton } from './AppButton';
import { DeviceCard } from './DeviceCard';
import { EmptyState } from './EmptyState';
import { LocationCard } from './LocationCard';
import { SectionHeader } from './SectionHeader';
import { SectionPager } from './SectionPager';
import { ArrowRight } from '../lib/ui/icons';
import { Text, XStack, YStack } from 'tamagui';

export interface HomeMonitoringWorkspaceProps {
  mode: HomeMonitoringMode;
  operationalFilter: HomeOperationalFilter;
  locations: LocationDetail[];
  locationPage: number;
  devicePage: number;
  locationPageSize: number;
  devicePageSize: number;
  selectedAttentionLocationId: string | null;
  onLocationPageChange: (page: number) => void;
  onDevicePageChange: (page: number) => void;
  onSelectAttentionLocation: (id: string) => void;
  onOpenLocation: (id: string) => void;
  onOpenDevice: (id: string) => void;
}

export function HomeMonitoringWorkspace({
  mode,
  operationalFilter,
  locations,
  locationPage,
  devicePage,
  locationPageSize,
  devicePageSize,
  selectedAttentionLocationId,
  onLocationPageChange,
  onDevicePageChange,
  onSelectAttentionLocation,
  onOpenLocation,
  onOpenDevice,
}: HomeMonitoringWorkspaceProps) {
  if (mode === 'devices') {
    const entries = flattenHomeDevices(locations);
    const current = paginate(entries, devicePage, devicePageSize);
    return (
      <YStack gap="$3">
        <SectionHeader
          title="Semua perangkat"
          description="Pantau perangkat dari seluruh lokasi dalam satu daftar."
          action={<SectionPager page={current.page} totalPages={current.totalPages} onPrevious={() => onDevicePageChange(Math.max(1, current.page - 1))} onNext={() => onDevicePageChange(Math.min(current.totalPages, current.page + 1))} />}
        />
        {current.items.length ? (
          <XStack gap="$3" flexWrap="wrap">
            {current.items.map(({ device, location }) => (
              <YStack key={device.id} width="100%" $tablet={{ width: 'calc(50% - 6px)' }} $wide={{ width: 'calc(33.333% - 8px)' }}>
                <DeviceCard device={device} locationName={location.name} onPress={() => onOpenDevice(device.id)} />
              </YStack>
            ))}
          </XStack>
        ) : (
          <EmptyState title="Belum ada perangkat" description="Perangkat akan muncul setelah terhubung ke lokasi." />
        )}
      </YStack>
    );
  }

  if (!operationalFilter) return null;
  const affectedLocations = getAffectedLocations(locations, operationalFilter);
  const locationPages = paginate(affectedLocations, locationPage, locationPageSize);
  const selected = affectedLocations.find((entry) => entry.location.id === selectedAttentionLocationId) ?? affectedLocations[0] ?? null;
  const selectedDevices = selected?.devices ?? [];
  const devicePages = paginate(selectedDevices, devicePage, devicePageSize);
  const label = getOperationalFilterLabel(operationalFilter);
  if (!affectedLocations.length) {
    return <EmptyState title={`Tidak ada ${label.toLowerCase()}`} description={operationalFilter === 'Angkat' ? 'Semua perangkat saat ini tidak memerlukan tindakan pengangkatan.' : 'Tidak ada perangkat dengan Safety Alert saat ini.'} />;
  }

  return (
    <YStack gap="$6">
      <YStack gap="$3">
        <SectionHeader
          title={label}
          description={operationalFilter === 'Angkat' ? 'Lokasi dan perangkat yang membutuhkan tindakan pengangkatan.' : 'Perangkat dengan kondisi keselamatan yang memerlukan perhatian segera.'}
          action={<SectionPager page={locationPages.page} totalPages={locationPages.totalPages} onPrevious={() => onLocationPageChange(Math.max(1, locationPages.page - 1))} onNext={() => onLocationPageChange(Math.min(locationPages.totalPages, locationPages.page + 1))} />}
        />
        <XStack gap="$3" flexWrap="wrap">
          {locationPages.items.map(({ location, devices }) => (
            <YStack key={location.id} width="100%" $tablet={{ width: 'calc(50% - 6px)' }} $wide={{ width: 'calc(25% - 9px)' }}>
              <LocationCard
                location={location}
                deviceCount={devices.length}
                selected={selectedAttentionLocationId === location.id}
                onPress={() => onSelectAttentionLocation(location.id)}
                onDetail={() => onOpenLocation(location.id)}
              />
            </YStack>
          ))}
        </XStack>
      </YStack>

      {selected ? (
        <YStack gap="$3">
          <SectionHeader
            title={`${operationalFilter === 'Angkat' ? 'Perangkat perlu diangkat di' : 'Peringatan di'} ${selected.location.name}`}
            description={operationalFilter === 'Angkat' ? 'Hanya perangkat dengan status Perlu diangkat yang ditampilkan.' : 'Hanya perangkat dengan Safety Alert yang ditampilkan.'}
            action={<XStack alignItems="center" gap="$2" flexWrap="wrap" justifyContent="flex-end"><Text color="$colorMuted" fontSize="$2" fontWeight="600">{selectedDevices.length} perangkat</Text><SectionPager page={devicePages.page} totalPages={devicePages.totalPages} onPrevious={() => onDevicePageChange(Math.max(1, devicePages.page - 1))} onNext={() => onDevicePageChange(Math.min(devicePages.totalPages, devicePages.page + 1))} /><AppButton variant="ghost" size="sm" onPress={() => onOpenLocation(selected.location.id)} ariaLabel="Lihat Detail">Lihat Detail <ArrowRight size={15} color="#0A9B62" strokeWidth={2.2} aria-hidden={true} /></AppButton></XStack>}
          />
          <XStack gap="$3" flexWrap="wrap">
            {devicePages.items.map((device) => (
              <YStack key={device.id} width="100%" $tablet={{ width: 'calc(50% - 6px)' }} $wide={{ width: 'calc(33.333% - 8px)' }}>
                <DeviceCard device={device} locationName={selected.location.name} onPress={() => onOpenDevice(device.id)} />
              </YStack>
            ))}
          </XStack>
        </YStack>
      ) : null}
    </YStack>
  );
}
