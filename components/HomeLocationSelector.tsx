import type { LocationListItem } from '../lib/api/types';
import { paginate } from '../lib/ui/pagination';
import { LocationCard } from './LocationCard';
import { EmptyState } from './EmptyState';
import { SectionHeader } from './SectionHeader';
import { SectionPager } from './SectionPager';
import { XStack, YStack } from 'tamagui';

export interface HomeLocationSelectorProps {
  locations: LocationListItem[];
  selectedLocationId: string | null;
  page: number;
  pageSize: number;
  onSelect: (id: string) => void;
  onOpenLocation: (id: string) => void;
  onPrevious: () => void;
  onNext: () => void;
}

export function HomeLocationSelector({
  locations,
  selectedLocationId,
  page,
  pageSize,
  onSelect,
  onOpenLocation,
  onPrevious,
  onNext,
}: HomeLocationSelectorProps) {
  const current = paginate(locations, page, pageSize);
  return (
    <YStack gap="$3">
      <SectionHeader
        title="Pantau per lokasi"
        description="Pilih lokasi untuk melihat perangkat yang terhubung."
        action={<SectionPager page={current.page} totalPages={current.totalPages} onPrevious={onPrevious} onNext={onNext} />}
      />
      {current.items.length ? (
        <XStack gap="$3" flexWrap="wrap">
          {current.items.map((location) => (
            <YStack key={location.id} width="100%" $tablet={{ width: 'calc(50% - 6px)' }} $wide={{ width: 'calc(25% - 9px)' }}>
              <LocationCard
                location={location}
                selected={selectedLocationId === location.id}
                onPress={() => onSelect(location.id)}
                onDetail={() => onOpenLocation(location.id)}
              />
            </YStack>
          ))}
        </XStack>
      ) : (
        <EmptyState title="Belum ada lokasi" description="Lokasi akan muncul setelah data monitoring tersedia." />
      )}
    </YStack>
  );
}
