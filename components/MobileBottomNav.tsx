import { Button, Text, XStack } from 'tamagui';
import type { LucideIcon } from 'lucide-react-native';

import type { NavigationItem } from './DesktopSidebar';
import { defaultNavigationItems } from './DesktopSidebar';
import { Clock3, House, MapPin } from '../lib/ui/icons';
import { textStyles } from '../lib/ui/typography';
import { useReducedMotion } from '../lib/ui/motionContext';

const navigationIcons: Record<string, LucideIcon> = {
  '/': House,
  '/locations': MapPin,
  '/history': Clock3,
};

export interface MobileBottomNavProps {
  items?: NavigationItem[];
  activePath?: string;
  onNavigate?: (path: string) => void;
}

export function MobileBottomNav({
  items = defaultNavigationItems,
  activePath = '/',
  onNavigate,
}: MobileBottomNavProps) {
  const reduceMotion = useReducedMotion();
  return (
    <XStack
      position="fixed"
      bottom={0}
      left={0}
      right={0}
      zIndex="$3"
      paddingHorizontal="$2"
      paddingTop="$2"
      paddingBottom="$3"
      gap="$2"
      backgroundColor="$surface"
      borderTopWidth={1}
      borderTopColor="$borderColor"
      role="navigation"
      aria-label="Navigasi utama"
    >
      {items.map((item) => {
        const active = activePath === item.path;
        const Icon = navigationIcons[item.path] ?? House;
        const displayLabel = item.label === 'Ringkasan' ? 'Beranda' : item.label;
        return (
          <Button
            key={item.path}
            flex={1}
            minHeight={54}
            paddingHorizontal="$2"
            backgroundColor={active ? '$primarySoft' : 'transparent'}
            borderRadius="$3"
            color={active ? '$primary' : '$colorMuted'}
            fontWeight={active ? '700' : '500'}
            onPress={() => onNavigate?.(item.path)}
            aria-label={`Buka ${displayLabel}`}
            type="button"
            transition={reduceMotion ? undefined : 'quick'}
            pressStyle={{ opacity: 0.76, scale: 0.98 }}
          >
            <XStack alignItems="center" justifyContent="center" gap="$2">
              <Icon size={17} color={active ? '#0A9B62' : '#5D7397'} strokeWidth={active ? 2.4 : 2} aria-hidden={true} />
              <Text {...textStyles.caption} color={active ? '$primary' : '$colorMuted'} fontWeight={active ? '700' : '600'}>
                {displayLabel}
              </Text>
            </XStack>
          </Button>
        );
      })}
    </XStack>
  );
}
