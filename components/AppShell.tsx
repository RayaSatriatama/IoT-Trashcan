import type { ReactNode } from 'react';
import { ScrollView } from 'react-native';
import { useMedia, XStack, YStack } from 'tamagui';

import { DesktopSidebar, type NavigationItem } from './DesktopSidebar';
import { Header } from './Header';
import { MobileBottomNav } from './MobileBottomNav';
import { SimulationBanner } from './SimulationBanner';

export interface AppShellProps {
  title: string;
  subtitle?: string;
  children: ReactNode;
  activePath?: string;
  navigationItems?: NavigationItem[];
  onNavigate?: (path: string) => void;
}

export function AppShell({
  title,
  subtitle,
  children,
  activePath = '/',
  navigationItems,
  onNavigate,
}: AppShellProps) {
  const media = useMedia();
  const isWide = Boolean(media.wide);

  return (
    <XStack flex={1} minHeight="100vh" backgroundColor="$background">
      {isWide ? (
        <DesktopSidebar items={navigationItems} activePath={activePath} onNavigate={onNavigate} />
      ) : null}
      <YStack flex={1} minWidth={0} minHeight={0}>
        <ScrollView
          style={{ flex: 1 }}
          contentContainerStyle={{ flexGrow: 1, paddingBottom: isWide ? 40 : 112 }}
          showsVerticalScrollIndicator
          keyboardShouldPersistTaps="handled"
        >
          <YStack width="100%" maxWidth={1320} alignSelf="center" paddingHorizontal={isWide ? '$7' : '$4'}>
            <Header title={title} subtitle={subtitle} />
            <SimulationBanner />
            {children}
          </YStack>
        </ScrollView>
      </YStack>
      {!isWide ? (
        <MobileBottomNav items={navigationItems} activePath={activePath} onNavigate={onNavigate} />
      ) : null}
    </XStack>
  );
}
