import { formatNullableMetric } from '../lib/format';
import { textStyles } from '../lib/ui/typography';
import { Text, XStack, YStack } from 'tamagui';
import { useAnimatedNumber } from '../lib/ui/useAnimatedNumber';

export type MetricBarTone = 'green' | 'blue' | 'red' | 'neutral';

export interface MetricBarProps {
  label: string;
  value: number | null | undefined;
  max?: number;
  unit?: string;
  unavailableLabel?: string;
  tone?: MetricBarTone;
  compact?: boolean;
  /** Optional control rendered next to the label, for example an edit button. */
  labelAction?: React.ReactNode;
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
  compact = false,
  labelAction,
}: MetricBarProps) {
  const validValue = typeof value === 'number' && Number.isFinite(value);
  // Kilograms keep two decimals so the bar agrees with the weight tile; other
  // units use one decimal only when the value is fractional.
  const precision = validValue ? (unit.includes('kg') ? 2 : Number.isInteger(value) ? 0 : 1) : 0;
  const animatedValue = useAnimatedNumber(validValue ? value : null, { precision });
  const visualValue = animatedValue ?? (validValue ? value : null);
  const percentage = visualValue !== null && max > 0 ? Math.min(100, Math.max(0, (visualValue / max) * 100)) : 0;
  const targetValueLabel = validValue ? formatMetricValue(value, unit) : unavailableLabel ?? 'Belum tersedia';
  const valueLabel = visualValue !== null ? formatMetricValue(visualValue, unit) : targetValueLabel;

  return (
    <YStack gap="$2" aria-label={`${label}: ${targetValueLabel}`}>
      <XStack justifyContent="space-between" alignItems="center" gap="$2">
        <XStack alignItems="center" gap="$1" minWidth={0}>
          <Text color="$colorMuted" {...textStyles.bodySecondary}>
            {label}
          </Text>
          {labelAction}
        </XStack>
        <Text color="$color" {...textStyles.bodySecondary} fontWeight="600">
          {valueLabel}
        </Text>
      </XStack>
      <YStack
        height={compact ? 8 : 10}
        borderRadius="$6"
        backgroundColor="$surfaceMuted"
        overflow="hidden"
        role="progressbar"
        aria-valuemin={0}
        aria-valuemax={max}
        aria-valuenow={validValue ? value : undefined}
        aria-valuetext={targetValueLabel}
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
