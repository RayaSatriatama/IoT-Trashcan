import { describe, expect, it, vi } from 'vitest';

vi.mock('expo-router', () => ({
  Link: ({ children }: { children: unknown }) => children,
  useRouter: () => ({ push: vi.fn() }),
}));

vi.mock('react-native', () => ({
  Image: () => null,
}));

import {
  HOME_KPI_LABELS,
  PRIORITY_EMPTY_COPY,
  RECENT_ACTIVITY_LIMIT,
  recentActivityItems,
} from '../../app/index';

describe('home route display contracts', () => {
  it('keeps the overview to four KPI labels', () => {
    expect(HOME_KPI_LABELS).toHaveLength(4);
    expect(new Set(HOME_KPI_LABELS).size).toBe(4);
  });

  it('caps recent activity at three items', () => {
    expect(RECENT_ACTIVITY_LIMIT).toBe(3);
    expect(recentActivityItems([1, 2, 3, 4])).toEqual([1, 2, 3]);
  });

  it('provides a specific empty priority copy', () => {
    expect(PRIORITY_EMPTY_COPY.title).toMatch(/prioritas/i);
    expect(PRIORITY_EMPTY_COPY.description).toBeTruthy();
  });
});
