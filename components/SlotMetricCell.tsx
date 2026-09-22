import type { ReactNode } from 'react';
import { textStyles } from '../lib/ui/typography';
import { Text, YStack } from 'tamagui';
import { formatNullableMetric } from '../lib/format';
import { useAnimatedNumber } from '../lib/ui/useAnimatedNumber';

export interface SlotMetricCellProps {
  icon?: ReactNode;
  value: string;
  label: string;
  accessibilityLabel?: string;
  numericValue?: number | null;
  unit?: string;
}

export function SlotMetricCell({ icon, value, label, accessibilityLabel, numericValue, unit = '' }: SlotMetricCellProps) {
  const validNumericValue = typeof numericValue === 'number' && Number.isFinite(numericValue);
  const precision = validNumericValue && !Number.isInteger(numericValue) ? 1 : 0;
  const animatedValue = useAnimatedNumber(validNumericValue ? numericValue : null, { precision });
  const displayValue = animatedValue === null ? value : formatNullableMetric(animatedValue, unit);
  return (
    <YStack
      flex={1}
      minWidth={0}
      alignItems="center"
      gap="$1"
      paddingHorizontal="$2"
      aria-label={accessibilityLabel ?? `${label}: ${value}`}
    >
      {icon}
      <Text {...textStyles.metricValue} color="$color" fontVariant={['tabular-nums']} textAlign="center">
        {displayValue}
      </Text>
      <Text {...textStyles.metricLabel} color="$colorMuted" textAlign="center" numberOfLines={1}>
        {label}
      </Text>
    </YStack>
  );
}
