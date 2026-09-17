import { Button, Separator, Text, XStack, YStack } from 'tamagui';

export interface NavigationItem {
  label: string;
  path: string;
}

export const defaultNavigationItems: NavigationItem[] = [
  { label: 'Ringkasan', path: '/' },
  { label: 'Lokasi', path: '/locations' },
  { label: 'Device', path: '/devices' },
];

export interface DesktopSidebarProps {
  items?: NavigationItem[];
  activePath?: string;
  onNavigate?: (path: string) => void;
}

export function DesktopSidebar({
  items = defaultNavigationItems,
  activePath = '/',
  onNavigate,
}: DesktopSidebarProps) {
  return (
    <YStack
      width={248}
      minHeight="100%"
      padding="$5"
      gap="$5"
      borderRightWidth={1}
      borderRightColor="$borderColor"
      backgroundColor="$surface"
    >
      <XStack alignItems="center" gap="$3">
        <YStack width={36} height={36} borderRadius="$2" backgroundColor="$primary" />
        <YStack gap="$1">
          <Text fontSize="$4" color="$color" fontWeight="700">
            SIGAP
          </Text>
          <Text fontSize="$1" color="$colorMuted">
            Monitoring
          </Text>
        </YStack>
      </XStack>
      <Separator borderColor="$borderColor" />
      <YStack gap="$2">
        {items.map((item) => {
          const active = activePath === item.path;
          return (
            <Button
              key={item.path}
              minHeight={48}
              width="100%"
              justifyContent="flex-start"
              paddingHorizontal="$3"
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
      </YStack>
    </YStack>
  );
}
