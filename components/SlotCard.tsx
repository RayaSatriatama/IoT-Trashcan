import b3Artwork from '../assets/bin_b3_single_faithful.png';
import inorganicArtwork from '../assets/bin_inorganic_single_faithful.png';
import organicArtwork from '../assets/bin_organic_single_faithful.png';
import type { SlotDTO } from '../lib/api/types';
import {
  buildSlotMetrics,
  getStreamTone,
  shouldShowDecisionNote,
} from '../lib/ui/slotPresentation';
import { textStyles } from '../lib/ui/typography';
import { Image as RNImage } from 'react-native';
import { Card, Text, XStack, YStack, useMedia } from 'tamagui';

import { getStreamLabel } from './DeviceCard';
import { MetricBar } from './MetricBar';
import { SlotFillGauge } from './SlotFillGauge';
import { SlotMetricCell } from './SlotMetricCell';
import { StatusBadge } from './StatusBadge';
import { Clock3, Scale, ShieldCheck, Thermometer, Wind } from '../lib/ui/icons';

const streamArtwork: Record<string, number> = {
  organic: organicArtwork,
  inorganic: inorganicArtwork,
  b3: b3Artwork,
};

const streamBorder: Record<string, string> = {
  green: '$primary',
  blue: '$blue',
  red: '$statusSafetyAlert',
  neutral: '$borderColor',
};

export interface SlotCardProps {
  slot: SlotDTO;
}

export function SlotCard({ slot }: SlotCardProps) {
  const media = useMedia();
  const tabletHeader = !media.compact && !media.largeDesktop;
  const tone = getStreamTone(slot.stream);
  const streamLabel = getStreamLabel(slot.stream);
  const metrics = buildSlotMetrics(slot);
  const metricIcons = slot.stream === 'b3'
    ? [<Scale key="weight" size={18} color="#5D7397" />, <Thermometer key="temperature" size={18} color="#5D7397" />, <ShieldCheck key="safety" size={18} color="#5D7397" />]
    : [<Scale key="weight" size={18} color="#5D7397" />, <Wind key="ogi" size={18} color="#5D7397" />, <Clock3 key="update" size={18} color="#5D7397" />];

  return (
    <Card
      borderWidth={1.5}
      borderColor={streamBorder[tone]}
      backgroundColor="$surface"
      borderRadius="$4"
      padding="$4"
      gap="$4"
      role="group"
      aria-label={`Slot ${streamLabel}`}
    >
      <XStack justifyContent="space-between" alignItems="center" gap="$3" flexWrap="wrap">
        <XStack alignItems="center" gap="$3" flex={1} minWidth={0} width={tabletHeader ? '100%' : undefined}>
          {streamArtwork[slot.stream] ? (
            <RNImage
              source={streamArtwork[slot.stream]}
              accessibilityLabel={`Ilustrasi slot ${streamLabel}`}
              resizeMode="contain"
              style={{ width: 54, height: 58 }}
            />
          ) : null}
          <YStack gap="$1" minWidth={0}>
            <Text color="$color" {...textStyles.cardTitle} $compact={textStyles.cardTitleCompact} fontWeight="800" numberOfLines={1}>
              {streamLabel}
            </Text>
            <Text color="$colorMuted" {...textStyles.metricLabel}>
              Slot {streamLabel.toLowerCase()}
            </Text>
          </YStack>
        </XStack>
        <YStack width={tabletHeader ? '100%' : undefined} alignItems={tabletHeader ? 'flex-start' : 'flex-end'}>
          <StatusBadge status={slot.status} />
        </YStack>
      </XStack>

      <XStack alignItems="center" gap="$4" flexWrap="wrap">
        <SlotFillGauge value={slot.fill} tone={tone} />
        <YStack gap="$2" flex={1} minWidth={140}>
          <Text {...textStyles.metricLabel} color="$colorMuted">
            Keterisian
          </Text>
          <Text {...textStyles.body} color="$color" fontWeight="700">
            {slot.status ?? '—'}
          </Text>
          <Text {...textStyles.bodySecondary} color="$colorMuted">
            Status operasional
          </Text>
        </YStack>
      </XStack>

      <MetricBar
        label="Utilisasi berat"
        value={slot.weight_utilization}
        tone={tone}
        compact
        unavailableLabel="—"
      />

      <XStack
        borderTopWidth={1}
        borderTopColor="$borderColor"
        paddingTop="$3"
        gap="$2"
        flexWrap="wrap"
      >
        {metrics.map((metric, index) => (
          <SlotMetricCell
            key={metric.label}
            icon={metricIcons[index]}
            value={metric.value}
            label={metric.label}
            accessibilityLabel={`${metric.accessibilityLabel}: ${metric.value}`}
          />
        ))}
      </XStack>

      {shouldShowDecisionNote(slot) ? (
        <YStack
          gap="$1"
          borderLeftWidth={3}
          borderLeftColor="$statusWaspada"
          backgroundColor="$statusWaspadaSoft"
          paddingHorizontal="$3"
          paddingVertical="$2"
          borderRadius="$2"
        >
          <Text {...textStyles.overline} color="$statusWaspada">
            ALASAN
          </Text>
          <Text {...textStyles.bodySecondary} color="$color" numberOfLines={2}>
            {slot.dominant_reason}
          </Text>
        </YStack>
      ) : null}
    </Card>
  );
}

export const SlotVisualCard = SlotCard;
