import type { ReactNode } from 'react';
import { Card, Text, YStack } from 'tamagui';

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
      backgroundColor="$surface"
      borderRadius="$3"
      padding="$6"
      gap="$2"
      alignItems="center"
      role="status"
      aria-label={title}
    >
      <YStack alignItems="center" gap="$2" maxWidth={520}>
        <Text fontSize="$4" color="$color" fontWeight="700" textAlign="center">
          {title}
        </Text>
        {description ? (
          <Text fontSize="$2" color="$colorMuted" textAlign="center">
            {description}
          </Text>
        ) : null}
        {action}
      </YStack>
    </Card>
  );
}
