import type { LocationListItem } from '../lib/api/types';
import { Image as RNImage } from 'react-native';
import { Button, Card, Text, XStack, YStack } from 'tamagui';

import { ChevronRight } from '../lib/ui/icons';
import { resolveLocationImage } from '../lib/ui/assets';
import { textStyles } from '../lib/ui/typography';
import { StatusBadge } from './StatusBadge';

export interface LocationCardProps {
  location: LocationListItem;
  deviceCount?: number | null;
  onPress?: () => void;
  onDetail?: () => void;
  selected?: boolean;
}

export function LocationCard({ location, deviceCount, onPress, onDetail, selected = false }: LocationCardProps) {
  const address = location.address_text ?? 'Belum tersedia';
  const deviceLabel = deviceCount === null || deviceCount === undefined ? 'Belum tersedia' : `${deviceCount}`;

  const cardContent = (
    <YStack gap="$3" flex={1}>
      <XStack alignItems="center" gap="$3" paddingRight="$3">
        <RNImage source={resolveLocationImage(location.name)} accessibilityLabel={`Ilustrasi ${location.name}`} resizeMode="contain" style={{ width: 64, height: 56 }} />
        <YStack flex={1} minWidth={0} gap="$1">
          <Text color="$color" {...textStyles.cardTitle} minHeight={48} numberOfLines={2}>{location.name}</Text>
          <Text color="$colorMuted" {...textStyles.bodySecondary} minHeight={44} numberOfLines={2}>{address}</Text>
        </YStack>
      </XStack>
      <XStack alignItems="center" gap="$2" marginTop="$1" flexWrap="wrap" rowGap="$1">
        <StatusBadge status={location.status} compact />
        {deviceLabel !== 'Belum tersedia' ? (
          <Text {...textStyles.caption} color="$colorMuted" flexShrink={0} numberOfLines={1}>
            {deviceLabel} device
          </Text>
        ) : null}
      </XStack>
    </YStack>
  );

  return (
    <Card
      borderWidth={selected ? 2 : 1}
      borderColor={selected ? '$primary' : '$borderColor'}
      backgroundColor={selected ? '$primarySoft' : '$surface'}
      borderRadius="$4"
      padding={0}
      minHeight={172}
      $compact={{ minHeight: 160 }}
      position="relative"
      overflow="hidden"
    >
      {onPress ? (
        <Button
          unstyled
          flex={1}
          alignItems="stretch"
          justifyContent="flex-start"
          flexDirection="column"
          padding="$4"
          paddingRight="$6"
          hoverStyle={{ backgroundColor: '$surfaceMuted' }}
          pressStyle={{ opacity: 0.82 }}
          onPress={onPress}
          role="button"
          aria-label={`Lokasi ${location.name}`}
          aria-pressed={selected}
          cursor="pointer"
          type="button"
        >
          {cardContent}
        </Button>
      ) : (
        <YStack flex={1} padding="$4" paddingRight="$6">
          {cardContent}
        </YStack>
      )}
      {onDetail ? (
        <Button
          unstyled
          position="absolute"
          right="$3"
          top="$3"
          zIndex={2}
          width={44}
          height={44}
          alignItems="center"
          justifyContent="center"
          borderRadius="$6"
          hoverStyle={{ backgroundColor: '$surfaceMuted' }}
          pressStyle={{ opacity: 0.7 }}
          onPress={onDetail}
          aria-label={`Lihat detail lokasi ${location.name}`}
          type="button"
        >
          <ChevronRight size={19} color="#5D7397" strokeWidth={2} aria-hidden={true} />
        </Button>
      ) : (
        <YStack position="absolute" right="$4" top="$4">
          <ChevronRight size={19} color="#5D7397" strokeWidth={2} aria-hidden={true} />
        </YStack>
      )}
    </Card>
  );
}

export const LocationVisualCard = LocationCard;
