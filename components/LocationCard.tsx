import type { LocationListItem } from '../lib/api/types';
import { Card, Text, XStack, YStack } from 'tamagui';

import { StatusBadge } from './StatusBadge';

export interface LocationCardProps {
  location: LocationListItem;
  deviceCount?: number | null;
  onPress?: () => void;
}

export function LocationCard({ location, deviceCount, onPress }: LocationCardProps) {
  const address = location.address_text ?? 'Belum tersedia';
  const deviceLabel = deviceCount === null || deviceCount === undefined ? 'Belum tersedia' : `${deviceCount}`;

  return (
    <Card
      borderWidth={1}
      borderColor="$borderColor"
      backgroundColor="$surface"
      borderRadius="$4"
      padding="$4"
      gap="$4"
      minHeight={166}
      pressStyle={onPress ? { opacity: 0.82 } : undefined}
      onPress={onPress}
      role={onPress ? 'button' : 'group'}
      aria-label={`Lokasi ${location.name}`}
      cursor={onPress ? 'pointer' : undefined}
    >
      <XStack justifyContent="space-between" alignItems="flex-start" gap="$3">
        <YStack flex={1} gap="$1">
          <Text fontSize="$5" color="$color" fontWeight="800" letterSpacing={-0.3}>
            {location.name}
          </Text>
          <Text fontSize="$2" color="$colorMuted">
            {address}
          </Text>
        </YStack>
        <StatusBadge status={location.status} compact />
      </XStack>
      <XStack justifyContent="space-between" alignItems="center" gap="$3" borderTopWidth={1} borderTopColor="$borderColor" paddingTop="$3">
        <Text fontSize="$2" color="$colorMuted">
          Perangkat terpantau
        </Text>
        <Text fontSize="$3" color="$color" fontWeight="600">
          {deviceLabel === 'Belum tersedia' ? deviceLabel : `${deviceLabel} device`}
        </Text>
      </XStack>
    </Card>
  );
}
