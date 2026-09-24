import { useState } from 'react';
import { Card, ScrollView, Text, Tooltip, XStack, YStack } from 'tamagui';

import { KpiCard } from '../KpiCard';
import { SectionHeader } from '../SectionHeader';
import type { HistorySummary, HistoryTrend, HistoryTrendBucket } from '../../lib/api/types';
import { Activity, Box, ShieldAlert, Truck } from '../../lib/ui/icons';
import { useReducedMotion } from '../../lib/ui/motionContext';
import { useAnimatedNumber } from '../../lib/ui/useAnimatedNumber';
import { textStyles } from '../../lib/ui/typography';

export interface HistoryAnalyticsProps {
  summary: HistorySummary;
  trend: HistoryTrend;
  compact: boolean;
}

export function trendBucketTotal(bucket: HistoryTrendBucket): number {
  return Math.max(0, Math.trunc(bucket.total));
}

export function trendBucketAccessibilityLabel(bucket: HistoryTrendBucket): string {
  return `${bucket.label}: total ${trendBucketTotal(bucket)}, Normal ${bucket.normal}, Waspada ${bucket.waspada}, Perlu diangkat ${bucket.pickup}, Safety Alert ${bucket.safety_alert}, Data tidak valid ${bucket.data_invalid}`;
}

const segments = [
  { key: 'normal', label: 'Normal', color: '#63AA6A' },
  { key: 'waspada', label: 'Waspada', color: '#F39A2D' },
  { key: 'pickup', label: 'Perlu diangkat', color: '#EF7D20' },
  { key: 'safety_alert', label: 'Safety Alert', color: '#EF4E58' },
  { key: 'data_invalid', label: 'Data tidak valid', color: '#7A5AA6' },
] as const;

function TrendBucketBar({ bucket, maximum }: { bucket: HistoryTrendBucket; maximum: number }) {
  const reduceMotion = useReducedMotion();
  const [detailsOpen, setDetailsOpen] = useState(false);
  const total = trendBucketTotal(bucket);
  const progress = total === 0 ? 0.03 : total / maximum;
  const animatedProgress = useAnimatedNumber(progress, { precision: 3 });
  const label = trendBucketAccessibilityLabel(bucket);
  return (
    <YStack flex={1} minWidth={52} alignItems="center" justifyContent="flex-end" gap="$2">
      <Tooltip delay={0} restMs={120} placement="top" allowFlip focus={{ enabled: true }}>
        <Tooltip.Trigger asChild>
          <YStack
            width="72%"
            maxWidth={42}
            minWidth={18}
            height={150}
            scaleY={animatedProgress ?? progress}
            transformOrigin="bottom"
            borderRadius="$3"
            overflow="hidden"
            backgroundColor="$surfaceMuted"
            role="button"
            tabIndex={0}
            aria-label={label}
            aria-expanded={detailsOpen}
            cursor="pointer"
            onPress={() => setDetailsOpen((value) => !value)}
            hoverStyle={{ opacity: 0.82 }}
            focusStyle={{ outlineColor: '$primary', outlineWidth: 2, outlineStyle: 'solid' }}
            transition={reduceMotion ? undefined : 'quick'}
          >
            {segments.map((segment) => (
              <YStack key={segment.key} flex={Math.max(0, bucket[segment.key])} backgroundColor={segment.color} />
            ))}
          </YStack>
        </Tooltip.Trigger>
        <Tooltip.Content elevate bordered borderColor="$borderColor" backgroundColor="$surface" padding="$3" role="tooltip">
          <Text color="$color" {...textStyles.caption}>{label}</Text>
        </Tooltip.Content>
      </Tooltip>
      <Text color="$colorMuted" {...textStyles.caption} numberOfLines={1}>{bucket.label}</Text>
      {detailsOpen ? (
        <YStack gap="$1" alignItems="center">
          <Text color="$colorMuted" {...textStyles.caption}>{total} data</Text>
          {segments.map((segment) => <Text key={segment.key} color="$colorMuted" {...textStyles.caption}>{segment.label}: {bucket[segment.key]}</Text>)}
        </YStack>
      ) : null}
    </YStack>
  );
}

function TrendLegend() {
  return (
    <XStack gap="$3" flexWrap="wrap">
      {segments.map((segment) => (
        <XStack key={segment.key} alignItems="center" gap="$1">
          <YStack width={8} height={8} borderRadius="$6" backgroundColor={segment.color} />
          <Text color="$colorMuted" {...textStyles.caption}>{segment.label}</Text>
        </XStack>
      ))}
    </XStack>
  );
}

function formatTrendScope(trend: HistoryTrend): string {
  if (trend.default_window) return '30 hari terakhir · waktu Asia/Jakarta';
  if (!trend.scope_from || !trend.scope_to) return 'Sesuai periode filter';
  return `${trend.scope_from.slice(0, 10)} – ${trend.scope_to.slice(0, 10)} · ${trend.granularity}`;
}

export function HistoryAnalytics({ summary, trend, compact }: HistoryAnalyticsProps) {
  const maximum = Math.max(...trend.buckets.map(trendBucketTotal), 1);
  const bucketContent = trend.buckets.length ? (
    <XStack alignItems="flex-end" gap={compact ? "$3" : "$2"} minHeight={202} minWidth={compact ? Math.max(320, trend.buckets.length * 58) : undefined} width="100%">
      {trend.buckets.map((bucket) => <TrendBucketBar key={`${bucket.start}-${bucket.label}`} bucket={bucket} maximum={maximum} />)}
    </XStack>
  ) : (
    <YStack minHeight={150} alignItems="center" justifyContent="center" gap="$1">
      <Text color="$color" {...textStyles.body} fontWeight="700">Belum ada data trend</Text>
      <Text color="$colorMuted" {...textStyles.caption}>Tidak ada pembacaan sesuai filter pada rentang ini.</Text>
    </YStack>
  );

  return (
    <YStack gap="$3">
      <Card borderWidth={1} borderColor="$borderColor" backgroundColor="$surface" borderRadius="$4" padding="$4" gap="$3">
        <XStack alignItems="center" justifyContent="space-between" gap="$3" flexWrap="wrap">
          <SectionHeader title="Trend status" description="Distribusi status dari seluruh pembacaan yang sesuai filter, bukan hanya halaman aktif." />
          <TrendLegend />
        </XStack>
        <Text color="$colorMuted" {...textStyles.caption}>{formatTrendScope(trend)}{trend.truncated ? ' · menampilkan bucket terbaru' : ''}</Text>
        {compact ? <ScrollView horizontal showsHorizontalScrollIndicator={false}>{bucketContent}</ScrollView> : (
          <XStack alignItems="stretch" gap="$2" width="100%">
            <YStack height={202} width={30} justifyContent="space-between" alignItems="flex-end" paddingVertical="$2">
              <Text color="$colorMuted" {...textStyles.caption}>{maximum}</Text>
              <Text color="$colorMuted" {...textStyles.caption}>0</Text>
            </YStack>
            <YStack flex={1}>{bucketContent}</YStack>
          </XStack>
        )}
      </Card>
      <XStack gap="$3" flexWrap="wrap">
        <YStack width="calc(50% - 6px)" $wide={{ width: 'calc(25% - 9px)' }}><KpiCard label="Hasil filter" description="Seluruh pembacaan yang sesuai filter" value={summary.total} icon={Activity} tone="blue" /></YStack>
        <YStack width="calc(50% - 6px)" $wide={{ width: 'calc(25% - 9px)' }}><KpiCard label="Normal" value={summary.normal} icon={Box} tone="green" /></YStack>
        <YStack width="calc(50% - 6px)" $wide={{ width: 'calc(25% - 9px)' }}><KpiCard label="Perlu diangkat" value={summary.pickup} icon={Truck} tone="orange" /></YStack>
        <YStack width="calc(50% - 6px)" $wide={{ width: 'calc(25% - 9px)' }}><KpiCard label="Safety Alert" value={summary.safety_alert} icon={ShieldAlert} tone="red" /></YStack>
      </XStack>
    </YStack>
  );
}
