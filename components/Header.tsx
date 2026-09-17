import type { ReactNode } from 'react';
import { Text, XStack, YStack } from 'tamagui';

export interface HeaderProps {
  title: string;
  subtitle?: string;
  actions?: ReactNode;
}

export function Header({ title, subtitle, actions }: HeaderProps) {
  return (
    <XStack
      alignItems="flex-start"
      justifyContent="space-between"
      gap="$4"
      paddingVertical="$5"
      flexWrap="wrap"
    >
      <YStack flex={1} minWidth={220} gap="$1">
        <Text fontSize="$7" lineHeight="$7" color="$color" fontWeight="700">
          {title}
        </Text>
        {subtitle ? (
          <Text fontSize="$3" color="$colorMuted">
            {subtitle}
          </Text>
        ) : null}
      </YStack>
      {actions ? <XStack alignItems="center" gap="$2">{actions}</XStack> : null}
    </XStack>
  );
}
