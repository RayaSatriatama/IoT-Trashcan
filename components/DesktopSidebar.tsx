import logoArtwork from '../assets/logo_mark_ui_generated.png';
import { Image as RNImage } from 'react-native';
import { Button, Separator, Text, XStack, YStack } from 'tamagui';

export interface NavigationItem {
  label: string;
  path: string;
}

export const defaultNavigationItems: NavigationItem[] = [
  { label: 'Ringkasan', path: '/' },
  { label: 'Lokasi', path: '/locations' },
  { label: 'Riwayat', path: '/history' },
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
      paddingHorizontal="$5"
      paddingTop="$6"
      paddingBottom="$5"
      gap="$6"
      borderRightWidth={1}
      borderRightColor="$borderColor"
      backgroundColor="$surface"
    >
      <XStack alignItems="center" gap="$3">
        <RNImage
          source={logoArtwork}
          accessibilityLabel="Logo SIGAP"
          resizeMode="contain"
          style={{ width: 44, height: 44 }}
        />
        <YStack gap="$1">
          <Text fontSize="$5" color="$color" fontWeight="800" letterSpacing={-0.5}>
            SIGAP
          </Text>
          <Text fontSize="$1" color="$colorMuted">
            Smart Bin Monitoring
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
              minHeight={52}
              width="100%"
              justifyContent="flex-start"
              paddingHorizontal="$4"
              borderRadius="$3"
              backgroundColor={active ? '$primarySoft' : 'transparent'}
              color={active ? '$primary' : '$colorMuted'}
              fontWeight={active ? '700' : '500'}
              onPress={() => onNavigate?.(item.path)}
              aria-label={`Buka ${item.label}`}
              type="button"
            >
              <Text color={active ? '$primary' : '$colorMuted'} fontSize="$2" fontWeight={active ? '800' : '600'}>
                {item.label === 'Ringkasan' ? 'Beranda' : item.label}
              </Text>
            </Button>
          );
        })}
      </YStack>
      <YStack flex={1} justifyContent="flex-end" gap="$1">
        <Text color="$primary" fontSize="$1" fontWeight="800">SIGAP</Text>
        <Text color="$colorMuted" fontSize="$1">Smart Bin Monitoring</Text>
        <Text color="$colorMuted" fontSize="$1">v1.0.0</Text>
      </YStack>
    </YStack>
  );
}
