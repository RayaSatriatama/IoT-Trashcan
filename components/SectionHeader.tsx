import type { ReactNode } from 'react';
import { Text, XStack, YStack } from 'tamagui';
import { textStyles } from '../lib/ui/typography';

export function SectionHeader({ title, description, action }: { title: string; description?: string; action?: ReactNode }) {
  return (
    <XStack alignItems="flex-start" justifyContent="space-between" gap="$3" flexWrap="wrap">
      <YStack gap="$1" flex={1} minWidth={220}>
        <Text color="$color" {...textStyles.sectionTitle}>{title}</Text>
        {description ? <Text color="$colorMuted" {...textStyles.bodySecondary}>{description}</Text> : null}
      </YStack>
      {action ? <XStack alignItems="center" gap="$2">{action}</XStack> : null}
    </XStack>
  );
}
