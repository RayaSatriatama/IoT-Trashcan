import { formatNullableMetric } from '../lib/format';
import { textStyles } from '../lib/ui/typography';
import { Text, XStack, YStack } from 'tamagui';

export type MetricBarTone = 'green' | 'blue' | 'red' | 'neutral';

export interface MetricBarProps {
  label: string;
  value: number | null | undefined;
  max?: number;
  unit?: string;
  unavailableLabel?: string;
  tone?: MetricBarTone;
}

const toneColor: Record<MetricBarTone, string> = {
  green: '$primary',
  blue: '$blue',
  red: '$statusSafetyAlert',
  neutral: '$colorMuted',
};

export function formatMetricValue(value: number | null | undefined, unit = ''): string {
  return formatNullableMetric(value, unit);
}

export function MetricBar({
  label,
  value,
  max = 100,
  unit = '%',
  unavailableLabel,
  tone = 'neutral',
}: MetricBarProps) {
  const validValue = typeof value === 'number' && Number.isFinite(value);
  const percentage = validValue && max > 0 ? Math.min(100, Math.max(0, (value / max) * 100)) : 0;
  const valueLabel = validValue ? formatMetricValue(value, unit) : unavailableLabel ?? 'Belum tersedia';

  return (
    <YStack gap="$2" aria-label={`${label}: ${valueLabel}`}>
      <XStack justifyContent="space-between" alignItems="center" gap="$2">
        <Text color="$colorMuted" {...textStyles.bodySecondary}>
          {label}
        </Text>
        <Text color="$color" {...textStyles.bodySecondary} fontWeight="600">
          {valueLabel}
        </Text>
      </XStack>
      <YStack
        height={10}
        borderRadius="$6"
        backgroundColor="$surfaceMuted"
        overflow="hidden"
        role="progressbar"
        aria-valuemin={0}
        aria-valuemax={max}
        aria-valuenow={validValue ? value : undefined}
        aria-valuetext={valueLabel}
      >
        {validValue ? (
          <YStack
            height="100%"
            width={`${percentage}%`}
            minWidth={percentage > 0 ? 2 : 0}
            backgroundColor={toneColor[tone]}
            borderRadius="$6"
          />
        ) : null}
      </YStack>
    </YStack>
  );
}
