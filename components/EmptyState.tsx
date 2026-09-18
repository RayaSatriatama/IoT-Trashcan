import type { ReactNode } from 'react';
import { Card, Text, YStack } from 'tamagui';
import { textStyles } from '../lib/ui/typography';

export interface EmptyStateProps {
  title: string;
  description?: string;
  action?: ReactNode;
}

export function EmptyState({ title, description, action }: EmptyStateProps) {
  return (
    <Card
      borderWidth={1}
      borderColor="$borderColor"
      borderRadius="$4"
      backgroundColor="$surfaceMuted"
      padding="$6"
      gap="$2"
      alignItems="center"
      role="status"
      aria-label={title}
    >
      <YStack alignItems="center" gap="$2" maxWidth={520}>
        <Text {...textStyles.cardTitle} color="$color" textAlign="center">
          {title}
        </Text>
        {description ? (
          <Text {...textStyles.bodySecondary} color="$colorMuted" textAlign="center">
            {description}
          </Text>
        ) : null}
        {action}
      </YStack>
    </Card>
  );
}
