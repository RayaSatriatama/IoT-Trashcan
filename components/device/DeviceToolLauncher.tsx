import type { ReactNode } from 'react';
import { Button, Text, XStack } from 'tamagui';
import { ChevronRight } from '../../lib/ui/icons';
import { textStyles } from '../../lib/ui/typography';

export function DeviceToolLauncher({ label, icon, blue = false, expanded, onPress }: {
  label: string; icon: ReactNode; blue?: boolean; expanded: boolean; onPress: () => void;
}) {
  return <Button type="button" aria-label={label} aria-expanded={expanded} onPress={onPress}
    flex={1} width="100%" minWidth={0} minHeight={70} padding={13} gap={10}
    borderWidth={1} borderColor="$borderColor" borderRadius={12} backgroundColor="$surface"
    justifyContent="flex-start" hoverStyle={{ backgroundColor: '$background' }}
    focusStyle={{ outlineColor: '$focusRing', outlineWidth: 3, outlineStyle: 'solid' }}>
    <XStack width={38} height={38} flexShrink={0} borderRadius={12} alignItems="center" justifyContent="center" backgroundColor={blue ? '$deviceToolBlue' : '$deviceToolGreen'}>{icon}</XStack>
    <Text color="$color" {...textStyles.cardTitle} fontSize={12} lineHeight={16} flex={1} minWidth={0} textAlign="left">{label}</Text>
    <ChevronRight size={16} color="#10275B" aria-hidden />
  </Button>;
}
