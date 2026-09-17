import { Button, XStack } from 'tamagui';

import type { NavigationItem } from './DesktopSidebar';
import { defaultNavigationItems } from './DesktopSidebar';

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
  return (
    <XStack
      position="fixed"
      bottom={0}
      left={0}
      right={0}
      zIndex="$3"
      paddingHorizontal="$2"
      paddingTop="$2"
      paddingBottom="$4"
      gap="$2"
      backgroundColor="$surface"
      borderTopWidth={1}
      borderTopColor="$borderColor"
      role="navigation"
      aria-label="Navigasi utama"
    >
      {items.map((item) => {
        const active = activePath === item.path;
        return (
          <Button
            key={item.path}
            flex={1}
            minHeight={48}
            paddingHorizontal="$2"
            backgroundColor={active ? '$primarySoft' : 'transparent'}
            color={active ? '$primary' : '$colorMuted'}
            fontWeight={active ? '700' : '500'}
            onPress={() => onNavigate?.(item.path)}
            aria-label={`Buka ${item.label}`}
            type="button"
          >
            {item.label}
          </Button>
        );
      })}
    </XStack>
  );
}
