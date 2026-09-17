import type { ReactNode } from 'react';
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
  const hasDesktopShell = Boolean(media.tablet);

  return (
    <XStack flex={1} minHeight="100vh" backgroundColor="$background">
      {hasDesktopShell ? (
        <DesktopSidebar items={navigationItems} activePath={activePath} onNavigate={onNavigate} />
      ) : null}
      <YStack flex={1} minWidth={0}>
        <YStack width="100%" maxWidth={1320} alignSelf="center" paddingHorizontal={hasDesktopShell ? '$7' : '$4'} paddingBottom={hasDesktopShell ? '$10' : 104}>
          <Header title={title} subtitle={subtitle} />
          <SimulationBanner />
          {children}
        </YStack>
      </YStack>
      {!hasDesktopShell ? (
        <MobileBottomNav items={navigationItems} activePath={activePath} onNavigate={onNavigate} />
      ) : null}
    </XStack>
  );
}
