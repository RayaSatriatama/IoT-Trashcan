import type { DeviceDTO } from '../lib/api/types';
import { formatTimestamp } from '../lib/format';
import b3Artwork from '../assets/bin_b3_single_faithful.png';
import inorganicArtwork from '../assets/bin_inorganic_single_faithful.png';
import organicArtwork from '../assets/bin_organic_single_faithful.png';
import { Image as RNImage } from 'react-native';
import { Card, Text, XStack, YStack } from 'tamagui';

import { MetricBar } from './MetricBar';
import { StatusBadge } from './StatusBadge';
import { textStyles } from '../lib/ui/typography';

export interface DeviceCardProps {
  device: DeviceDTO;
  onPress?: () => void;
}

const streamLabels: Record<string, string> = {
  organic: 'Organik',
  inorganic: 'Anorganik',
  b3: 'B3',
};

export function getStreamLabel(stream: string): string {
  return streamLabels[stream] ?? stream;
}

export function DeviceCard({ device, onPress }: DeviceCardProps) {
  const onlineLabel = device.online_state ?? 'Belum tersedia';
  const lastSeen = formatTimestamp(device.last_seen);

  return (
    <Card
      borderWidth={1}
      borderColor="$borderColor"
      backgroundColor="$surface"
      borderRadius="$4"
      padding="$4"
      gap="$4"
      pressStyle={onPress ? { opacity: 0.82 } : undefined}
      onPress={onPress}
      role={onPress ? 'button' : 'group'}
      aria-label={`Device ${device.device_code}`}
      cursor={onPress ? 'pointer' : undefined}
    >
      <XStack justifyContent="space-between" alignItems="flex-start" gap="$3">
        <XStack alignItems="center" gap="$1" backgroundColor="$surfaceMuted" borderRadius="$3" paddingHorizontal="$2" paddingVertical="$1">
          <RNImage source={organicArtwork} accessibilityLabel="Ilustrasi slot organik" resizeMode="contain" style={{ width: 31, height: 38 }} />
          <RNImage source={inorganicArtwork} accessibilityLabel="Ilustrasi slot anorganik" resizeMode="contain" style={{ width: 31, height: 38 }} />
          <RNImage source={b3Artwork} accessibilityLabel="Ilustrasi slot B3" resizeMode="contain" style={{ width: 31, height: 38 }} />
        </XStack>
        <YStack flex={1} gap="$1">
          <Text color="$color" {...textStyles.cardTitle}>
            {device.device_code}
          </Text>
          <Text color="$colorMuted" {...textStyles.bodySecondary}>
            {device.placement_label ?? 'Belum tersedia'}
          </Text>
        </YStack>
        <StatusBadge status={device.status} compact />
      </XStack>
      <XStack flexWrap="wrap" gap="$2" alignItems="center" paddingVertical="$1">
        <Text color="$colorMuted" {...textStyles.bodySecondary}>
          Koneksi: {onlineLabel}
        </Text>
        <Text color="$colorMuted" {...textStyles.bodySecondary}>
          Terakhir terlihat: {lastSeen}
        </Text>
      </XStack>
      <YStack gap="$3" borderTopWidth={1} borderTopColor="$borderColor" paddingTop="$3">
        {device.slots.length > 0 ? (
          device.slots.map((slot) => (
            <MetricBar key={slot.stream} label={getStreamLabel(slot.stream)} value={slot.fill} />
          ))
        ) : (
          <Text color="$colorMuted" {...textStyles.bodySecondary}>
            Belum ada data slot
          </Text>
        )}
      </YStack>
    </Card>
  );
}

export const DeviceVisualCard = DeviceCard;
