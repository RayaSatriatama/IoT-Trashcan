import type { SlotDTO } from '../lib/api/types';
import { formatNullableMetric, formatTimestamp } from '../lib/format';
import { Card, Text, XStack, YStack } from 'tamagui';

import { getStreamLabel } from './DeviceCard';
import { MetricBar } from './MetricBar';
import { StatusBadge } from './StatusBadge';

export interface SlotCardProps {
  slot: SlotDTO;
}

export function SlotCard({ slot }: SlotCardProps) {
  return (
    <Card
      borderWidth={1}
      borderColor="$borderColor"
      backgroundColor="$surface"
      borderRadius="$3"
      padding="$4"
      gap="$4"
      role="group"
      aria-label={`Slot ${getStreamLabel(slot.stream)}`}
    >
      <XStack justifyContent="space-between" alignItems="center" gap="$3">
        <Text fontSize="$4" color="$color" fontWeight="700">
          {getStreamLabel(slot.stream)}
        </Text>
        <StatusBadge status={slot.status} compact />
      </XStack>
      <YStack gap="$3">
        <MetricBar label="Keterisian" value={slot.fill} />
        <MetricBar label="Utilisasi berat" value={slot.weight_utilization} />
      </YStack>
      <YStack gap="$2">
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
