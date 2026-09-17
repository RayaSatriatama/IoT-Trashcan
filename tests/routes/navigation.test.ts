import { describe, expect, it, vi } from 'vitest';

vi.mock('react-native', () => ({
  Image: () => null,
}));

vi.mock('tamagui', () => ({
  Button: () => null,
  Separator: () => null,
  Text: () => null,
  XStack: () => null,
  YStack: () => null,
}));

import { defaultNavigationItems } from '../../components/DesktopSidebar';

describe('primary navigation', () => {
  it('includes the audit history destination', () => {
    expect(defaultNavigationItems).toContainEqual({ label: 'Riwayat', path: '/history' });
  });
});
