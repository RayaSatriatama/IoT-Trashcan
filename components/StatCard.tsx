import type { ReactNode } from 'react';
import { Card, Text, XStack, YStack } from 'tamagui';
import { Image as RNImage } from 'react-native';

export interface StatCardProps {
  label: string;
  value: ReactNode;
  description?: string;
  accent?: string;
  icon?: number;
}

export function StatCard({ label, value, description, accent = '$primary', icon }: StatCardProps) {
  const accentSoft = accent === '$statusAngkat' ? '$statusAngkatSoft' : accent === '$statusSafetyAlert' ? '$statusSafetyAlertSoft' : accent === '$blue' ? '$blueSoft' : '$primarySoft';
  return (
    <Card
      borderWidth={1}
      borderColor="$borderColor"
      backgroundColor="$surface"
      borderRadius="$4"
      padding="$4"
      gap="$3"
      minHeight={150}
      overflow="hidden"
      role="group"
      aria-label={label}
    >
      <XStack alignItems="center" justifyContent="space-between" gap="$3">
        <YStack gap="$1" flex={1}>
          <Text fontSize="$2" color="$colorMuted" fontWeight="700">
          {label}
          </Text>
          <Text fontSize="$8" lineHeight="$8" color={accent} fontWeight="800" letterSpacing={-1}>
            {value}
          </Text>
        </YStack>
        {icon ? (
          <YStack width={48} height={48} borderRadius="$6" backgroundColor={accentSoft} alignItems="center" justifyContent="center">
            <RNImage source={icon} resizeMode="contain" accessibilityElementsHidden style={{ width: 34, height: 34 }} />
          </YStack>
        ) : null}
      </XStack>
      {description ? (
        <Text fontSize="$2" color="$colorMuted">
          {description}
        </Text>
      ) : null}
    </Card>
  );
}
