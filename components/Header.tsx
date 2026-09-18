import type { ReactNode } from 'react';
import { Image as RNImage } from 'react-native';
import { Button, Popover, PopoverContent, PopoverTrigger, Separator, Text, XStack, YStack, useMedia } from 'tamagui';

import logoArtwork from '../assets/logo_mark_ui_generated.png';
import { Bell, CircleUserRound } from '../lib/ui/icons';
import { textStyles } from '../lib/ui/typography';
import { SimulationIndicator } from './SimulationIndicator';

export interface HeaderProps {
  title: string;
  subtitle?: string;
  actions?: ReactNode;
}

export function Header({ actions }: HeaderProps) {
  const media = useMedia();

  return (
    <YStack gap="$2" paddingTop="$3" paddingBottom="$2">
      <XStack
        alignItems="center"
        justifyContent={media.compact ? 'space-between' : 'flex-end'}
        gap="$3"
        minHeight={48}
      >
        {media.compact ? (
          <XStack alignItems="center" gap="$2" minWidth={0}>
            <RNImage
              source={logoArtwork}
              accessibilityLabel="Logo SIGAP"
              resizeMode="contain"
              style={{ width: 34, height: 34 }}
            />
            <Text color="$color" {...textStyles.cardTitle} fontSize={18} fontWeight="800">
              SIGAP
            </Text>
          </XStack>
        ) : null}

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
          {actions ? <XStack alignItems="center" gap="$2">{actions}</XStack> : null}
        </XStack>
      </XStack>
      <Separator borderColor="$borderColor" opacity={0.75} />
    </YStack>
  );
}
