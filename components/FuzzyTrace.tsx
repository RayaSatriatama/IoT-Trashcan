import { Card, Separator, Text, XStack, YStack } from 'tamagui';

import type { FuzzyTraceDTO } from '../lib/api/types';
import { getTraceInputRows, formatResearchNumber } from '../lib/ui/slotPresentation';
import { textStyles } from '../lib/ui/typography';

export function FuzzyTrace({ trace }: { trace: FuzzyTraceDTO | null | undefined }) {
  if (!trace) return null;
  const rows = getTraceInputRows(trace);
  return (
    <Card borderWidth={1} borderColor="$borderColor" backgroundColor="$background" borderRadius="$3" padding="$3" gap="$3">
      <Text {...textStyles.cardTitle} color="$color">Fuzzy trace</Text>
      <YStack gap="$1">
        {rows.map((row) => (
          <XStack key={row.key} alignItems="center" gap="$2" paddingVertical="$1">
            <Text {...textStyles.caption} color="$colorMuted" width={130}>{row.label}</Text>
            <Text {...textStyles.caption} color="$color" fontWeight="700" width={74} fontVariant={['tabular-nums']}>{row.value}</Text>
            <Text {...textStyles.caption} color="$colorMuted" flex={1}>{row.membership}</Text>
          </XStack>
        ))}
      </YStack>
      <Separator borderColor="$borderColor" />
      <XStack gap="$4" flexWrap="wrap">
        <YStack gap="$1" minWidth={110}><Text {...textStyles.caption} color="$colorMuted">Rules aktif</Text><Text {...textStyles.bodySecondary} color="$color" fontWeight="700">{trace.triggered_rules || '—'}</Text></YStack>
        <YStack gap="$1" minWidth={110}><Text {...textStyles.caption} color="$colorMuted">Skor prioritas</Text><Text {...textStyles.bodySecondary} color="$color" fontWeight="700" fontVariant={['tabular-nums']}>{formatResearchNumber(trace.score, ' / 100')}</Text></YStack>
        <YStack gap="$1" flex={1} minWidth={150}><Text {...textStyles.caption} color="$colorMuted">Status keputusan</Text><Text {...textStyles.bodySecondary} color="$color" fontWeight="700">{trace.status || 'Belum tersedia'}</Text></YStack>
      </XStack>
      {trace.dominant_reason ? <Text {...textStyles.bodySecondary} color="$colorMuted">Alasan: {trace.dominant_reason}</Text> : null}
      {trace.version ? <Text {...textStyles.caption} color="$colorMuted">FIS version: {trace.version}</Text> : null}
    </Card>
  );
}
