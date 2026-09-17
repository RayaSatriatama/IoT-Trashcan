import b3Artwork from '../assets/bin_b3_single_faithful.png';
import inorganicArtwork from '../assets/bin_inorganic_single_faithful.png';
import organicArtwork from '../assets/bin_organic_single_faithful.png';
import type { SlotDTO } from '../lib/api/types';
import { formatNullableMetric, formatTimestamp } from '../lib/format';
import { Image as RNImage } from 'react-native';
import { Card, Text, XStack, YStack } from 'tamagui';

import { getStreamLabel } from './DeviceCard';
import { MetricBar, type MetricBarTone } from './MetricBar';
import { StatusBadge } from './StatusBadge';

const streamArtwork: Record<string, number> = {
  organic: organicArtwork,
  inorganic: inorganicArtwork,
  b3: b3Artwork,
};

const streamTone: Record<string, MetricBarTone> = {
  organic: 'green',
  inorganic: 'blue',
  b3: 'red',
};

export interface SlotCardProps {
  slot: SlotDTO;
}

export function SlotCard({ slot }: SlotCardProps) {
  const tone = streamTone[slot.stream] ?? 'neutral';
  return (
    <Card
      borderWidth={1}
      borderColor="$borderColor"
      backgroundColor="$surface"
      borderRadius="$4"
      padding="$4"
      gap="$4"
      minHeight={360}
      role="group"
      aria-label={`Slot ${getStreamLabel(slot.stream)}`}
    >
      <XStack justifyContent="space-between" alignItems="center" gap="$3">
        <XStack alignItems="center" gap="$3" flex={1}>
          {streamArtwork[slot.stream] ? (
            <RNImage
              source={streamArtwork[slot.stream]}
              accessibilityLabel={`Ilustrasi slot ${getStreamLabel(slot.stream)}`}
              resizeMode="contain"
              style={{ width: 72, height: 72 }}
            />
          ) : null}
          <Text fontSize="$5" color="$color" fontWeight="800">
            {getStreamLabel(slot.stream)}
          </Text>
        </XStack>
        <StatusBadge status={slot.status} compact />
      </XStack>
      <YStack gap="$3">
        <MetricBar label="Keterisian" value={slot.fill} tone={tone} />
        <MetricBar label="Utilisasi berat" value={slot.weight_utilization} tone={tone} />
      </YStack>
      <YStack gap="$2" borderTopWidth={1} borderTopColor="$borderColor" paddingTop="$3">
        <MetricLine label="Berat" value={formatNullableMetric(slot.weight, ' kg')} />
        <MetricLine label="OGI" value={formatNullableMetric(slot.ogi)} />
        <MetricLine label="Akumulasi" value={formatNullableMetric(slot.accumulation_hours, ' jam')} />
        <MetricLine label="Suhu" value={formatNullableMetric(slot.temperature, ' °C')} />
        <MetricLine label="Perekaman" value={formatTimestamp(slot.recorded_at)} />
      </YStack>
      {slot.dominant_reason ? <MetricLine label="Alasan" value={slot.dominant_reason} /> : null}
      {slot.safety_state ? <MetricLine label="Keselamatan" value={slot.safety_state} /> : null}
    </Card>
  );
}

export const SlotVisualCard = SlotCard;

function MetricLine({ label, value }: { label: string; value: string }) {
  return (
    <XStack justifyContent="space-between" alignItems="flex-start" gap="$3">
      <Text fontSize="$2" color="$colorMuted">
        {label}
      </Text>
      <Text fontSize="$2" color="$color" fontWeight="600" textAlign="right" flex={1}>
        {value}
      </Text>
    </XStack>
  );
}
