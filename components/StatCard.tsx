import type { ReactNode } from 'react';
import { Card, Text, YStack } from 'tamagui';

export interface StatCardProps {
  label: string;
  value: ReactNode;
  description?: string;
  accent?: string;
}

export function StatCard({ label, value, description, accent = '$primary' }: StatCardProps) {
  return (
    <Card
      borderWidth={1}
      borderColor="$borderColor"
      backgroundColor="$surface"
      borderRadius="$3"
      padding="$4"
      gap="$2"
      minHeight={128}
      role="group"
      aria-label={label}
    >
      <YStack gap="$1">
        <Text fontSize="$2" color="$colorMuted" fontWeight="600">
          {label}
        </Text>
        <Text fontSize="$7" lineHeight="$7" color={accent} fontWeight="700">
          {value}
        </Text>
        {description ? (
          <Text fontSize="$2" color="$colorMuted">
            {description}
          </Text>
        ) : null}
      </YStack>
    </Card>
  );
}
