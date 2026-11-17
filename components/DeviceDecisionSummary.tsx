import { Card, Text, XStack, YStack } from 'tamagui';

import type { DeviceDecisionSummaryDTO, FirmwareResult } from '../lib/api/types';
import { bestFirmwareDecision } from '../lib/ui/firmwarePresentation';
import { formatDecisionSummary } from '../lib/ui/slotPresentation';
import { textStyles } from '../lib/ui/typography';
import { useAnimatedNumber } from '../lib/ui/useAnimatedNumber';
import { Activity, ShieldAlert, SlidersHorizontal } from '../lib/ui/icons';

function safeSafetyLabel(result: FirmwareResult) {
  if (result.safety.state === 'CLEAR' && result.safety.model_state === 'TEST_ONLY') return 'Prototipe · tanpa indikasi ambang';
  if (result.safety.state === 'CLEAR' && result.safety.model_state !== 'PRODUCTION') return 'Belum dinilai';
  return ({ CLEAR: 'Aman', ALERT: 'Peringatan keselamatan', FAULT: 'Gangguan', UNCONFIGURED: 'Belum dikonfigurasi', UNAVAILABLE: 'Belum tersedia' } as Record<string, string>)[result.safety.state] ?? 'Belum tersedia';
}

function SummaryItem({ icon, label, value, detail, valueTestID }: { icon: React.ReactNode; label: string; value: string; detail?: string; valueTestID?: string }) {
  return (
    <XStack alignItems="flex-start" gap="$3" flex={1} minWidth={170}>
      <XStack width={36} height={36} borderRadius={999} backgroundColor="$surfaceMuted" alignItems="center" justifyContent="center">
        {icon}
      </XStack>
      <YStack gap="$1" flex={1} minWidth={0}>
        <Text {...textStyles.caption} color="$colorMuted">{label}</Text>
        <Text {...textStyles.metricValue} color="$color" numberOfLines={2} testID={valueTestID}>{value}</Text>
        {detail ? <Text {...textStyles.caption} color="$colorMuted" numberOfLines={2}>{detail}</Text> : null}
      </YStack>
    </XStack>
  );
}

const isValidDecisionScore = (score: number | null | undefined): score is number =>
  typeof score === 'number' && Number.isFinite(score) && score >= 0 && score <= 100;

export function DeviceDecisionSummary({ summary, firmwareResults, webScore = null, webStatus = null }: {
  summary: DeviceDecisionSummaryDTO | null | undefined; firmwareResults?: readonly FirmwareResult[];
  /** Priority score computed from the four configured inputs when the MCU sends none. */
  webScore?: number | null; webStatus?: string | null;
}) {
  const organic = bestFirmwareDecision((firmwareResults ?? []).filter(result => result.stream === 'organic'));
  const formatted = formatDecisionSummary(summary);
  const sourceScore = organic
    ? organic.fuzzy.state === 'READY'
      && (organic.fuzzy.model_state === 'PRODUCTION' || organic.fuzzy.model_state === 'TEST_ONLY')
      && isValidDecisionScore(organic.fuzzy.score)
      ? organic.fuzzy.score
      : null
    : isValidDecisionScore(summary?.priority_score) ? summary.priority_score : isValidDecisionScore(webScore) ? webScore : null;
  const score = useAnimatedNumber(sourceScore ?? 0, { precision: 0 });

  return (
    <YStack gap="$2" flex={1} minWidth={320}>
      <Text {...textStyles.cardTitle} color="$color">Ringkasan keputusan</Text>
      <XStack gap="$4" flexWrap="wrap" paddingTop="$2">
        <SummaryItem icon={<Activity size={18} color="#2487E8" aria-hidden />} label="Prioritas angkut" value={`${Math.round(score ?? 0)} / 100`} valueTestID={sourceScore === null ? 'decision-summary-priority-initial' : undefined} detail={organic?.fuzzy.model_state === 'TEST_ONLY' && sourceScore !== null ? 'Prototipe · bukan keputusan operasional.' : undefined} />
        <SummaryItem icon={<ShieldAlert size={18} color="#D77A19" aria-hidden />} label="Slot paling kritis" value="Organik" detail={organic ? safeSafetyLabel(organic) : formatted.reason === '—' ? undefined : formatted.reason} />
        <SummaryItem icon={<SlidersHorizontal size={18} color="#0A9B62" aria-hidden />} label="Mode keputusan" value="Penilaian multi-parameter" detail={webStatus ? `Kategori: ${webStatus}` : undefined} />
      </XStack>
    </YStack>
  );
}

export function DeviceDecisionSummaryCard({ summary }: { summary: DeviceDecisionSummaryDTO | null | undefined }) {
  return <Card borderWidth={1} borderColor="$borderColor" backgroundColor="$surface" borderRadius="$4" padding="$4"><DeviceDecisionSummary summary={summary} /></Card>;
}
