import { formatNullableMetric } from '../lib/format';
import { Text, XStack, YStack } from 'tamagui';

export interface MetricBarProps {
  label: string;
  value: number | null | undefined;
  max?: number;
  unit?: string;
  unavailableLabel?: string;
}

export function formatMetricValue(value: number | null | undefined, unit = ''): string {
  return formatNullableMetric(value, unit);
}

export function MetricBar({
  label,
  value,
  max = 100,
  unit = '%',
  unavailableLabel,
}: MetricBarProps) {
  const validValue = typeof value === 'number' && Number.isFinite(value);
  const percentage = validValue && max > 0 ? Math.min(100, Math.max(0, (value / max) * 100)) : 0;
  const valueLabel = validValue ? formatMetricValue(value, unit) : unavailableLabel ?? 'Belum tersedia';

  return (
    <YStack gap="$2" aria-label={`${label}: ${valueLabel}`}>
      <XStack justifyContent="space-between" alignItems="center" gap="$2">
        <Text fontSize="$2" color="$colorMuted">
          {label}
        </Text>
        <Text fontSize="$2" color="$color" fontWeight="600">
          {valueLabel}
        </Text>
      </XStack>
      <YStack
        height={8}
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
            backgroundColor="$primary"
            borderRadius="$6"
          />
        ) : null}
      </YStack>
    </YStack>
  );
}
