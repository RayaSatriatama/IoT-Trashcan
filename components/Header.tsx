import type { ReactNode } from 'react';
import { Separator, Text, XStack, YStack } from 'tamagui';
import { Bell } from '../lib/ui/icons';

export interface HeaderProps {
  title: string;
  subtitle?: string;
  actions?: ReactNode;
}

export function Header({ title, subtitle, actions }: HeaderProps) {
  return (
    <YStack gap="$4" paddingTop="$6" paddingBottom="$4">
      <XStack alignItems="center" justifyContent="space-between" gap="$4" flexWrap="wrap">
        <YStack flex={1} minWidth={220} gap="$1">
          <Text fontSize="$7" lineHeight="$7" color="$color" fontWeight="800" letterSpacing={-0.8}>
            {title}
          </Text>
          {subtitle ? (
            <Text fontSize="$3" color="$colorMuted" lineHeight="$4">
              {subtitle}
            </Text>
          ) : null}
        </YStack>
        <XStack alignItems="center" gap="$3">
          <YStack
            width={44}
            height={44}
            borderRadius="$6"
            backgroundColor="$surface"
            borderWidth={1}
            borderColor="$borderColor"
            alignItems="center"
            justifyContent="center"
            aria-hidden={true}
          >
            <Bell size={18} color="#0A9B62" strokeWidth={2.2} aria-hidden={true} />
          </YStack>
          <YStack paddingLeft="$3" borderLeftWidth={1} borderLeftColor="$borderColor" gap="$1">
            <Text color="$color" fontSize="$2" fontWeight="800">Admin</Text>
            <Text color="$colorMuted" fontSize="$1">Pengelola sistem</Text>
          </YStack>
        </XStack>
        {actions ? <XStack alignItems="center" gap="$2">{actions}</XStack> : null}
      </XStack>
      <Separator borderColor="$borderColor" opacity={0.75} />
    </YStack>
  );
}
