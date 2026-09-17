# Core Monitoring Frontend Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build the responsive SIGAP monitoring frontend with Expo Router, React Native Web, Tamagui, real read APIs, polling, and the four Task 03 routes.

**Architecture:** A root Expo Router app renders shared Tamagui components. A typed fetch layer mirrors the existing Go DTOs and a single polling hook handles initial loading, retries, cancellation, stale data, and the 20-second interval. The web build remains a development artifact in Task 03; Go static embedding is deferred.

**Tech Stack:** Expo SDK selected by the current Expo CLI, React, React Native, React Native Web, Expo Router, Tamagui, TypeScript, native `fetch`.

**Spec:** `docs/superpowers/specs/2026-09-17-core-monitoring-frontend-design.md`

## Global Constraints

- Production remains one Go process; the frontend must not add a second backend.
- Web API requests use same-origin relative paths; no hard-coded host or ingest credential.
- Polling interval is 20 seconds, with no overlapping requests and cancellation on unmount.
- Canonical status values remain `Normal`, `Waspada`, `Angkat`, `Safety Alert`, `Offline`, and `Data Invalid`.
- Null metrics remain unavailable in the UI and are never converted to zero.
- Approved screenshots guide composition; no whole-page screenshot may be rendered as the UI.
- Only runtime assets needed by these screens are copied from `docs/SIGAP_Asset_Pack_Complete_v0.6`.
- History, CSV, full research presentation, Go embedding, PWA, login, maps, notifications, and write UI are out of scope.

---

### Task 1: Expo and Tamagui foundation

**Files:**
- Create: `package.json`
- Create: `app.json`
- Create: `tsconfig.json`
- Create: `babel.config.js`
- Create: `tamagui.config.ts`
- Create: `app/_layout.tsx`
- Create: `assets/logo_mark_ui_generated.png`
- Create: `assets/home_hero_smart_bin_network.png`
- Create: `assets/location_footer_multi_device_network.png`
- Create: `assets/device_detail_three_bin_with_device_card.png`
- Create: `assets/bin_organic_single_faithful.png`
- Create: `assets/bin_inorganic_single_faithful.png`
- Create: `assets/bin_b3_single_faithful.png`

**Interfaces:**
- Produces the Expo Router root and `TamaguiProvider` used by every screen.
- Produces media keys for compact and wide layouts and status tokens for shared components.

- [ ] **Step 1: Add the minimal Expo manifest and scripts**

Use Expo Router's plugin and scripts for `start`, `web`, `typecheck`, and `export:web`. Select compatible package versions through the current Expo CLI rather than manually guessing versions.

- [ ] **Step 2: Add Tamagui tokens and provider**

Define light-theme tokens for surfaces, text, borders, spacing, radii, and canonical statuses. Wrap `Slot` in `TamaguiProvider` from `app/_layout.tsx`.

- [ ] **Step 3: Copy only the listed runtime assets**

Copy the exact approved PNG files into `assets/`; leave the 50MB-plus documentation pack in `docs/`.

- [ ] **Step 4: Verify the foundation**

Run `npm install`, `npm run typecheck`, and `npx expo export --platform web`. Expected result: dependency installation, TypeScript success, and a successful web export without tracked `web-dist` output.

- [ ] **Step 5: Commit**

```bash
git add package.json package-lock.json app.json tsconfig.json babel.config.js tamagui.config.ts app/_layout.tsx assets
git commit -m "feat: bootstrap SIGAP monitoring frontend"
```

### Task 2: Typed API client and polling state

**Files:**
- Create: `lib/api/types.ts`
- Create: `lib/api/client.ts`
- Create: `lib/api/hooks.ts`
- Create: `lib/status.ts`
- Create: `lib/format.ts`
- Test: `lib/api/client.test.ts`
- Test: `lib/status.test.ts`
- Modify: `package.json` only if the chosen test script is needed

**Interfaces:**
- `fetchOverview(signal?: AbortSignal): Promise<OverviewResponse>`
- `fetchLocations(params: { query?: string; status?: CanonicalStatus }, signal?: AbortSignal): Promise<LocationListResponse>`
- `fetchLocation(id: string, signal?: AbortSignal): Promise<LocationDetail>`
- `fetchDevice(id: string, signal?: AbortSignal): Promise<DeviceDetail>`
- `usePollingResource<T>(loader: (signal: AbortSignal) => Promise<T>, intervalMs?: number)` returns `{ data, loading, error, stale, retry }`.

- [ ] **Step 1: Write client and mapping tests**

Cover successful typed JSON, non-2xx JSON error text, canonical status labels, and nullable metric formatting. Use a real small fetch adapter seam rather than coupling screen tests to transport internals.

- [ ] **Step 2: Run the focused tests red**

Run `npm test -- --runInBand lib/api/client.test.ts lib/status.test.ts`. Expected result: failure because client, status, and formatting modules do not yet exist.

- [ ] **Step 3: Implement DTO types and fetch functions**

Mirror the Go DTO fields exactly, construct relative paths with `URLSearchParams`, reject malformed/non-JSON responses with a typed `ApiError`, and support `AbortSignal`.

- [ ] **Step 4: Implement polling and presentation helpers**

Use a single in-flight request ref, abort the prior request before retry, clear the interval on unmount, retain successful data after later failures, and map statuses to Indonesian labels plus stable tokens.

- [ ] **Step 5: Run focused tests green**

Run `npm test -- --runInBand lib/api/client.test.ts lib/status.test.ts`. Expected result: all focused tests pass.

- [ ] **Step 6: Commit**

```bash
git add lib package.json package-lock.json
git commit -m "feat: add typed monitoring API client"
```

### Task 3: Shared shell, status, and state components

**Files:**
- Create: `components/AppShell.tsx`
- Create: `components/Header.tsx`
- Create: `components/DesktopSidebar.tsx`
- Create: `components/MobileBottomNav.tsx`
- Create: `components/StatusBadge.tsx`
- Create: `components/StatCard.tsx`
- Create: `components/LocationCard.tsx`
- Create: `components/DeviceCard.tsx`
- Create: `components/SlotCard.tsx`
- Create: `components/MetricBar.tsx`
- Create: `components/LoadingState.tsx`
- Create: `components/EmptyState.tsx`
- Create: `components/ErrorState.tsx`

**Interfaces:**
- `AppShell` accepts `title`, `subtitle`, and `children` and selects sidebar or bottom navigation from Tamagui media.
- `StatusBadge` accepts a canonical status and renders label, marker, and accessible text.
- `SlotCard` accepts a typed slot DTO and renders nullable metrics without inventing values.

- [ ] **Step 1: Write presentation tests**

Cover the status mapping and null metric output through the existing lightweight test setup. Keep tests focused on observable text and labels.

- [ ] **Step 2: Run focused tests red**

Run `npm test -- --runInBand components`. Expected result: failure until shared components exist.

- [ ] **Step 3: Implement tokens-based shared components**

Build cards, badges, progress bars, skeletons, empty/error states, and shell navigation with Tamagui primitives. Use accessible labels and minimum 44px touch targets.

- [ ] **Step 4: Run tests green and typecheck**

Run `npm test -- --runInBand components` and `npm run typecheck`.

- [ ] **Step 5: Commit**

```bash
git add components
git commit -m "feat: add responsive monitoring shell components"
```

### Task 4: Home and Locations routes

**Files:**
- Create: `app/index.tsx`
- Create: `app/locations/index.tsx`

**Interfaces:**
- Home loads `fetchOverview` with `usePollingResource` and links priority locations to `/locations/[id]`.
- Locations calls `fetchLocations` with `q` and canonical `status`, preserving the last successful list during polling failure.

- [ ] **Step 1: Add route tests or render checks for KPI and filter behavior**

Assert four KPI labels maximum, priority empty copy, recent activity maximum three, search input accessibility, and status filter values.

- [ ] **Step 2: Run the route checks red**

Run `npm test -- --runInBand app` and confirm missing route behavior fails for the expected reason.

- [ ] **Step 3: Implement Home**

Render a hero/illustration, four KPI cards, priority locations, recent activity, loading skeleton, empty priority copy, initial error retry, and stale polling notice.

- [ ] **Step 4: Implement Locations**

Render search, status filter, responsive location cards, empty search copy, initial error retry, and stale polling notice. Use Expo Router links for detail navigation.

- [ ] **Step 5: Verify routes green**

Run focused route tests, `npm run typecheck`, and `npx expo export --platform web`.

- [ ] **Step 6: Commit**

```bash
git add app/index.tsx app/locations/index.tsx
git commit -m "feat: add overview and locations screens"
```

### Task 5: Location and Device detail routes

**Files:**
- Create: `app/locations/[id].tsx`
- Create: `app/devices/[id].tsx`

**Interfaces:**
- Location detail calls `fetchLocation(id)` and links each device to `/devices/[id]`.
- Device detail calls `fetchDevice(id)` and renders exactly the API's available slots, expected to include organic, inorganic, and b3.

- [ ] **Step 1: Add detail route checks**

Cover device link targets, location empty-device copy, three stream labels, offline badge, Data Invalid badge, and null metric copy.

- [ ] **Step 2: Run detail checks red**

Run the focused route test command and confirm the missing routes fail for the expected reason.

- [ ] **Step 3: Implement detail screens**

Use shared cards, back navigation, identity metadata, last-seen formatting, aggregate status, slot metrics, dominant reason, safety state, and explicit unavailable values.

- [ ] **Step 4: Verify detail screens green**

Run focused tests, `npm run typecheck`, and `npx expo export --platform web`.

- [ ] **Step 5: Commit**

```bash
git add 'app/locations/[id].tsx' 'app/devices/[id].tsx'
git commit -m "feat: add location and device detail screens"
```

### Task 6: Browser, backend, and release verification

**Files:**
- Modify: `README.md` only if exact frontend commands need documenting.
- Create outside repository: temporary screenshots and `/tmp/sigap-task03-backend`.

- [ ] **Step 1: Start the Go API with a temporary seeded database**

Use a disposable temp directory, not `sigap.db` in the repository. Seed public location/device/slot/reading rows with the existing Go test helpers or a temporary SQL script.

- [ ] **Step 2: Start Expo web against the local API**

Use the configured web script and same-origin development proxy or the documented native base URL only for local development. Do not add a production host to source.

- [ ] **Step 3: Inspect 390x844 and 1366x768**

Check Home, Locations, Location Detail, and Device Detail for clipping, API success, navigation, assets, status labels, skeleton/error/empty behavior, sidebar/bottom nav, and console errors.

- [ ] **Step 4: Run frontend verification**

Run `npm run typecheck` and `npx expo export --platform web`. Confirm no `node_modules`, `.expo`, `web-dist`, database, secret, or screenshot enters the diff.

- [ ] **Step 5: Run backend regression**

Run `go test ./... -count=1`, `go vet ./...`, and `go build -o /tmp/sigap-task03-backend ./cmd/sigap`; remove only the temporary binary created outside the repository.

- [ ] **Step 6: Inspect and commit final documentation only when README changed**

Run `git status --short`, `git diff`, and `git diff --check`; stage explicit paths only. Commit documentation separately if changed.

- [ ] **Step 7: Push and create the pull request**

```bash
git push -u origin feat/core-monitoring-frontend
gh pr create --fill
```

The PR body must list screens, API endpoints, responsive viewports, approved references/assets, UI states, export/typecheck/backend verification, and Task 04 deferrals. Do not merge the PR.
