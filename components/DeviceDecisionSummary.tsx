import { Card, Text, XStack, YStack } from 'tamagui';

import type { DeviceDecisionSummaryDTO } from '../lib/api/types';
import { formatDecisionSummary } from '../lib/ui/slotPresentation';
import { textStyles } from '../lib/ui/typography';
import { useAnimatedNumber } from '../lib/ui/useAnimatedNumber';
import { Activity, ShieldAlert, SlidersHorizontal } from '../lib/ui/icons';

function SummaryItem({ icon, label, value, detail }: { icon: React.ReactNode; label: string; value: string; detail?: string }) {
  return (
    <XStack alignItems="flex-start" gap="$3" flex={1} minWidth={170}>
      <XStack width={36} height={36} borderRadius={999} backgroundColor="$surfaceMuted" alignItems="center" justifyContent="center">
        {icon}
      </XStack>
      <YStack gap="$1" flex={1} minWidth={0}>
        <Text {...textStyles.caption} color="$colorMuted">{label}</Text>
        <Text {...textStyles.metricValue} color="$color" numberOfLines={2}>{value}</Text>
        {detail ? <Text {...textStyles.caption} color="$colorMuted" numberOfLines={2}>{detail}</Text> : null}
      </YStack>
    </XStack>
  );
}

export function DeviceDecisionSummary({ summary }: { summary: DeviceDecisionSummaryDTO | null | undefined }) {
  const formatted = formatDecisionSummary(summary);
  const animatedScore = useAnimatedNumber(summary?.priority_score ?? null, { precision: 0 });
  const score = animatedScore === null ? formatted.score : `${Math.round(animatedScore)} / 100`;

  return (
    <YStack gap="$2" flex={1} minWidth={320}>
      <Text {...textStyles.cardTitle} color="$color">Ringkasan keputusan</Text>
      <XStack gap="$4" flexWrap="wrap" paddingTop="$2">
        <SummaryItem icon={<Activity size={18} color="#2487E8" aria-hidden />} label="Prioritas angkut" value={score} detail={summary?.priority_score === null || summary?.priority_score === undefined ? 'Belum tersedia dari API' : undefined} />
        <SummaryItem icon={<ShieldAlert size={18} color="#D77A19" aria-hidden />} label="Slot paling kritis" value={formatted.criticalStream} detail={formatted.reason === '—' ? undefined : formatted.reason} />
        <SummaryItem icon={<SlidersHorizontal size={18} color="#0A9B62" aria-hidden />} label="Mode keputusan" value={formatted.mode} />
      </XStack>
    </YStack>
  );
}

export function DeviceDecisionSummaryCard({ summary }: { summary: DeviceDecisionSummaryDTO | null | undefined }) {
  return <Card borderWidth={1} borderColor="$borderColor" backgroundColor="$surface" borderRadius="$4" padding="$4"><DeviceDecisionSummary summary={summary} /></Card>;
}
