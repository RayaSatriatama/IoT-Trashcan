import logoArtwork from '../assets/logo_mark_ui_generated.png';
import { Image as RNImage } from 'react-native';
import type { LucideIcon } from 'lucide-react-native';
import { Button, Separator, Text, XStack, YStack } from 'tamagui';
import { Clock3, House, MapPin } from '../lib/ui/icons';

export interface NavigationItem {
  label: string;
  path: string;
}

export const defaultNavigationItems: NavigationItem[] = [
  { label: 'Ringkasan', path: '/' },
  { label: 'Lokasi', path: '/locations' },
  { label: 'Riwayat', path: '/history' },
];

const navigationIcons: Record<string, LucideIcon> = {
  '/': House,
  '/locations': MapPin,
  '/history': Clock3,
};

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
      width={216}
      minHeight="100%"
      paddingHorizontal="$5"
      paddingTop="$6"
      paddingBottom="$5"
      gap="$5"
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
          const Icon = navigationIcons[item.path] ?? House;
          const displayLabel = item.label === 'Ringkasan' ? 'Beranda' : item.label;
          return (
            <Button
              key={item.path}
              minHeight={48}
              width="100%"
              justifyContent="flex-start"
              paddingHorizontal="$3"
              borderRadius="$3"
              backgroundColor={active ? '$primarySoft' : 'transparent'}
              color={active ? '$primary' : '$colorMuted'}
              fontWeight={active ? '700' : '500'}
              onPress={() => onNavigate?.(item.path)}
              aria-label={`Buka ${displayLabel}`}
              type="button"
            >
              <XStack alignItems="center" gap="$3">
                <Icon size={18} color={active ? '#0A9B62' : '#5D7397'} strokeWidth={active ? 2.4 : 2} aria-hidden={true} />
                <Text color={active ? '$primary' : '$colorMuted'} fontSize="$2" fontWeight={active ? '800' : '600'}>
                  {displayLabel}
                </Text>
              </XStack>
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
