import type { LocationListItem } from '../lib/api/types';
import { Image as RNImage } from 'react-native';
import { Card, Text, XStack, YStack } from 'tamagui';

import { ChevronRight } from '../lib/ui/icons';
import { resolveLocationImage } from '../lib/ui/assets';
import { textStyles } from '../lib/ui/typography';
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
      gap="$3"
      minHeight={112}
      overflow="hidden"
      pressStyle={onPress ? { opacity: 0.82 } : undefined}
      onPress={onPress}
      role={onPress ? 'button' : 'group'}
      aria-label={`Lokasi ${location.name}`}
      cursor={onPress ? 'pointer' : undefined}
    >
      <XStack alignItems="center" gap="$3">
        <RNImage source={resolveLocationImage(location.name)} accessibilityLabel={`Ilustrasi ${location.name}`} resizeMode="contain" style={{ width: 90, height: 72 }} />
        <YStack flex={1} minWidth={0} gap="$1">
          <Text color="$color" {...textStyles.cardTitle} numberOfLines={1}>{location.name}</Text>
          <Text color="$colorMuted" {...textStyles.bodySecondary} numberOfLines={2}>{address}</Text>
          <XStack alignItems="center" gap="$2" marginTop="$1">
            <StatusBadge status={location.status} compact />
            {deviceLabel !== 'Belum tersedia' ? <Text {...textStyles.caption} color="$colorMuted">{deviceLabel} device</Text> : null}
          </XStack>
        </YStack>
        <ChevronRight size={19} color="#5D7397" strokeWidth={2} aria-hidden={true} />
      </XStack>
    </Card>
  );
}

export const LocationVisualCard = LocationCard;
