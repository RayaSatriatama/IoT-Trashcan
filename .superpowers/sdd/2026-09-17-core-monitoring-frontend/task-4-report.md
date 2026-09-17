# Task 4 report: Home and Locations routes

## Status

PASS

## Changes

- Added the Home route with the approved hero asset, exactly four KPI cards, priority location links, three-item recent activity cap, loading, initial error/retry, empty, and stale polling states.
- Added the Locations route with accessible search, canonical status filtering, responsive location cards, detail links, loading, initial error/retry, empty-search, and stale polling states.
- Added route contract tests under `tests/routes/` for KPI count, priority empty copy, recent activity limit, search accessibility, and canonical status values.

## Verification

- `npm test`: PASS, 18 tests.
- `npm run typecheck`: PASS.
- `npx expo export --platform web`: PASS. Static routes are `/`, `/locations`, `/_sitemap`, and `/+not-found`.

## Scope notes

Loaders are `useCallback`-stable and use the real `apiClient` plus `usePollingResource`; no runtime mocks were added. Co-located `app/*.test.ts` files were avoided because Expo Router exposes every file in `app/` as a static route, so route tests live in `tests/routes/`.
