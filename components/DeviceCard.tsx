import type { DeviceDTO } from '../lib/api/types';
import { formatTimestamp, optionalText } from '../lib/format';
import { getStreamTone } from '../lib/ui/slotPresentation';
import b3Artwork from '../assets/bin_b3_single_faithful.png';
import inorganicArtwork from '../assets/bin_inorganic_single_faithful.png';
import organicArtwork from '../assets/bin_organic_single_faithful.png';
import { Image as RNImage } from 'react-native';
import { Card, Text, XStack, YStack } from 'tamagui';

import { MetricBar } from './MetricBar';
import { FirmwareMonitoringPanel } from './device/FirmwareMonitoringPanel';
import { FirmwareResultsPanel } from './device/FirmwareResultsPanel';
import { StatusBadge } from './StatusBadge';
import { textStyles } from '../lib/ui/typography';
import { useReducedMotion } from '../lib/ui/motionContext';

export interface DeviceCardProps {
  device: DeviceDTO;
  onPress?: () => void;
  locationName?: string;
}

const streamLabels: Record<string, string> = {
  organic: 'Organik',
  inorganic: 'Anorganik',
  b3: 'B3',
};

export function getStreamLabel(stream: string): string {
  return streamLabels[stream] ?? stream;
}

export function DeviceCard({ device, onPress, locationName }: DeviceCardProps) {
  const reduceMotion = useReducedMotion();
  const onlineLabel = device.online_state ?? 'Belum tersedia';
  const lastSeen = formatTimestamp(device.last_seen);
  const deviceContext = [optionalText(locationName), optionalText(device.placement_label)].filter(Boolean).join(' · ');

  return (
    <Card
      borderWidth={1}
      borderColor="$borderColor"
      backgroundColor="$surface"
      borderRadius="$4"
      padding="$4"
      gap="$4"
      minHeight={330}
      pressStyle={onPress ? { opacity: 0.82 } : undefined}
      onPress={onPress}
      role={onPress ? 'button' : 'group'}
      aria-label={`Device ${device.device_code}`}
      cursor={onPress ? 'pointer' : undefined}
      transition={reduceMotion ? undefined : 'standard'}
      enterStyle={reduceMotion ? undefined : { opacity: 0, y: 4 }}
      hoverStyle={onPress ? { borderColor: '$primary', y: -2 } : undefined}
    >
      <XStack justifyContent="space-between" alignItems="flex-start" gap="$3">
        <XStack alignItems="center" gap="$1" backgroundColor="$surfaceMuted" borderRadius="$3" paddingHorizontal="$2" paddingVertical="$1">
          <RNImage source={organicArtwork} accessibilityLabel="Ilustrasi slot organik" resizeMode="contain" style={{ width: 31, height: 38 }} />
          <RNImage source={inorganicArtwork} accessibilityLabel="Ilustrasi slot anorganik" resizeMode="contain" style={{ width: 31, height: 38 }} />
          <RNImage source={b3Artwork} accessibilityLabel="Ilustrasi slot B3" resizeMode="contain" style={{ width: 31, height: 38 }} />
        </XStack>
        <YStack flex={1} minWidth={0} gap="$1" alignItems="stretch">
          <Text color="$color" {...textStyles.cardTitle} $compact={textStyles.cardTitleCompact} alignSelf="stretch" textAlign="left" minHeight={28} numberOfLines={1} ellipsizeMode="tail">
            {device.device_code}
          </Text>
          {deviceContext ? <Text color="$colorMuted" {...textStyles.bodySecondary} alignSelf="stretch" textAlign="left" numberOfLines={2}>{deviceContext}</Text> : null}
        </YStack>
      </XStack>
      <XStack flexWrap="wrap" gap="$2" alignItems="center" justifyContent="space-between" paddingVertical="$1">
        <XStack flex={1} minWidth={0} flexWrap="wrap" gap="$2" alignItems="center">
          <Text color={device.online_state === 'Offline' ? '$statusOffline' : '$statusNormal'} {...textStyles.bodySecondary} fontWeight="700">
            {onlineLabel}
          </Text>
          <Text color="$colorMuted" {...textStyles.metadata} numberOfLines={1}>
            Update: {lastSeen}
          </Text>
        </XStack>
        <StatusBadge status={device.status} compact />
      </XStack>
      <YStack gap="$3" borderTopWidth={1} borderTopColor="$borderColor" paddingTop="$3">
        {device.slots.length > 0 ? (
          device.slots.map((slot) => (
            <MetricBar key={slot.stream} label={getStreamLabel(slot.stream)} value={slot.fill} tone={getStreamTone(slot.stream)} />
          ))
        ) : device.firmware_monitoring?.length ? null : (
          <Text color="$colorMuted" {...textStyles.bodySecondary}>
            Belum ada data slot
          </Text>
        )}
      </YStack>
      {device.firmware_monitoring?.length ? <FirmwareMonitoringPanel samples={device.firmware_monitoring} compact /> : null}
      {device.firmware_results?.length ? <FirmwareResultsPanel results={device.firmware_results} compact /> : null}
    </Card>
  );
}

export const DeviceVisualCard = DeviceCard;
