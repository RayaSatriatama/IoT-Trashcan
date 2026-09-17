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
      borderRadius="$3"
      padding="$4"
      gap="$3"
      pressStyle={onPress ? { opacity: 0.82 } : undefined}
      onPress={onPress}
      role={onPress ? 'button' : 'group'}
      aria-label={`Lokasi ${location.name}`}
      cursor={onPress ? 'pointer' : undefined}
    >
      <XStack justifyContent="space-between" alignItems="flex-start" gap="$3">
        <YStack flex={1} gap="$1">
          <Text fontSize="$4" color="$color" fontWeight="700">
            {location.name}
          </Text>
          <Text fontSize="$2" color="$colorMuted">
            {address}
          </Text>
        </YStack>
        <StatusBadge status={location.status} compact />
      </XStack>
      <XStack justifyContent="space-between" alignItems="center" gap="$3">
        <Text fontSize="$2" color="$colorMuted">
          Device
        </Text>
        <Text fontSize="$3" color="$color" fontWeight="600">
          {deviceLabel}
        </Text>
      </XStack>
    </Card>
  );
}
