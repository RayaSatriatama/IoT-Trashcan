import type { ReactNode } from 'react';
import { Button, Popover, PopoverContent, PopoverTrigger, Separator, Text, XStack, YStack, useMedia } from 'tamagui';
import { Bell, CircleUserRound } from '../lib/ui/icons';
import { textStyles } from '../lib/ui/typography';
import { SimulationIndicator } from './SimulationIndicator';

export interface HeaderProps {
  title: string;
  subtitle?: string;
  actions?: ReactNode;
}

export function Header({ title, subtitle, actions }: HeaderProps) {
  const media = useMedia();
  return (
    <YStack gap="$4" paddingTop="$6" paddingBottom="$4">
      <XStack alignItems="center" justifyContent="space-between" gap="$4" flexWrap="wrap">
        <YStack flex={1} minWidth={220} gap="$1">
          <Text
            fontFamily="$body"
            fontSize={media.wide ? textStyles.pageTitle.fontSize : 30}
            lineHeight={media.wide ? textStyles.pageTitle.lineHeight : 36}
            color="$color"
            fontWeight="800"
            letterSpacing={-0.8}
          >
            {title}
          </Text>
          {subtitle ? (
            <Text color="$colorMuted" {...textStyles.bodySecondary}>
              {subtitle}
            </Text>
          ) : null}
        </YStack>
        <XStack alignItems="center" gap="$2">
          <SimulationIndicator />
          <Button
            minWidth={44}
            minHeight={44}
            padding={0}
            borderRadius="$6"
            backgroundColor="$surface"
            borderWidth={1}
            borderColor="$borderColor"
            hoverStyle={{ borderColor: '$primary' }}
            aria-label="Notifikasi"
            type="button"
          >
            <Bell size={18} color="#0A9B62" strokeWidth={2.2} aria-hidden={true} />
          </Button>
          <Popover placement="bottom-end" allowFlip>
            <PopoverTrigger asChild>
              <Button
                minWidth={44}
                minHeight={44}
                padding={0}
                borderRadius="$6"
                backgroundColor="$surface"
                borderWidth={1}
                borderColor="$borderColor"
                hoverStyle={{ borderColor: '$primary' }}
                aria-label="Buka profil Admin"
                type="button"
              >
                <CircleUserRound size={20} color="#0A9B62" strokeWidth={2.1} aria-hidden={true} />
              </Button>
            </PopoverTrigger>
            <PopoverContent elevate bordered borderColor="$borderColor" backgroundColor="$surface" padding="$3" width={190}>
              <YStack gap="$1">
                <Text color="$color" {...textStyles.bodySecondary} fontWeight="800">Admin</Text>
                <Text color="$colorMuted" {...textStyles.caption}>Pengelola sistem</Text>
              </YStack>
            </PopoverContent>
          </Popover>
          <YStack display={media.compact ? 'none' : 'flex'} paddingLeft="$3" borderLeftWidth={1} borderLeftColor="$borderColor" gap="$1">
            <Text color="$color" {...textStyles.bodySecondary} fontWeight="800">Admin</Text>
            <Text color="$colorMuted" {...textStyles.metricLabel}>Pengelola sistem</Text>
          </YStack>
        </XStack>
        {actions ? <XStack alignItems="center" gap="$2">{actions}</XStack> : null}
      </XStack>
      <Separator borderColor="$borderColor" opacity={0.75} />
    </YStack>
  );
}
