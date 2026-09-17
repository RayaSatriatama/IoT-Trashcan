import type { ReactNode } from 'react';
import { textStyles } from '../lib/ui/typography';
import { Text, YStack } from 'tamagui';

export interface SlotMetricCellProps {
  icon?: ReactNode;
  value: string;
  label: string;
  accessibilityLabel?: string;
}

export function SlotMetricCell({ icon, value, label, accessibilityLabel }: SlotMetricCellProps) {
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
        {value}
      </Text>
      <Text {...textStyles.metricLabel} color="$colorMuted" textAlign="center" numberOfLines={1}>
        {label}
      </Text>
    </YStack>
  );
}
