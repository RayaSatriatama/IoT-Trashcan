import type { ReactNode } from 'react';
import { useMedia, XStack, YStack } from 'tamagui';

import { DesktopSidebar, type NavigationItem } from './DesktopSidebar';
import { Header } from './Header';
import { MobileBottomNav } from './MobileBottomNav';

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
      <YStack flex={1} minWidth={0}>
        <YStack width="100%" maxWidth={1280} alignSelf="center" paddingHorizontal="$4" paddingBottom={isWide ? '$10' : 96}>
          <Header title={title} subtitle={subtitle} />
          {children}
        </YStack>
      </YStack>
      {!isWide ? (
        <MobileBottomNav items={navigationItems} activePath={activePath} onNavigate={onNavigate} />
      ) : null}
    </XStack>
  );
}
