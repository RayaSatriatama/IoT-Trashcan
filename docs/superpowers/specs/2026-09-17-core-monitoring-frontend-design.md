# SIGAP Core Monitoring Frontend Design

## Goal

Add the first universal SIGAP monitoring UI on top of the existing public read APIs. The web target uses Expo, React Native Web, Expo Router, and Tamagui, with real same-origin API data and no runtime mock responses.

## Scope

The feature covers four routes:

- `/`: overview KPI cards, priority locations, and up to three recent events.
- `/locations`: searchable and status-filterable location list.
- `/locations/[id]`: location identity, aggregate status, devices, and slot summaries.
- `/devices/[id]`: device identity, online state, three slot cards, and operational metrics.

History, CSV, the full research panel, static Go embedding, authentication, maps, notifications, charts, and write/configuration UI remain deferred.

## Architecture

The app is a small Expo Router project at the repository root. `app/` owns routes, `components/` owns reusable presentation, and `lib/api/` owns typed fetch functions plus a polling hook. `tamagui.config.ts` defines the visual tokens and responsive media query. `TamaguiProvider` wraps the router in `app/_layout.tsx`.

The API client uses relative URLs in web builds. It accepts an optional native base URL from `EXPO_PUBLIC_API_BASE_URL`, normalizes JSON errors, and supports `AbortSignal`. `usePollingResource` performs one initial request, polls every 20 seconds, cancels on unmount or replacement, retains the last successful value after a polling failure, and exposes retry/stale state.

## API contract mapping

TypeScript types mirror the current Go DTOs:

- `GET /api/overview` returns `total_locations`, `total_devices`, `pickup_required`, `safety_alerts`, `priority_locations`, and `recent_activity`.
- `GET /api/locations` returns `{ locations: [{ id, name, address_text, status }] }`.
- `GET /api/locations/:id` returns location fields plus `devices`.
- `GET /api/devices/:id` returns device identity/state plus `slots`.
- Each slot exposes `stream`, nullable `fill`, `weight`, `weight_utilization`, `status`, `dominant_reason`, `safety_state`, and `recorded_at`.

The research endpoint is not rendered as a full panel in this task.

## Visual system

The Tamagui configuration defines background, surface, muted surface, text, muted text, border, radius, spacing, and canonical status colors. Status components always render a text label and a visual marker, so color is not the only signal. The shell uses a desktop sidebar at wide media sizes and a fixed mobile bottom navigation at compact sizes. Content is centered at approximately 1280px with one-column mobile cards and multi-column desktop cards.

Approved concept screenshots are references only. Runtime UI is composed from cards, labels, controls, progress bars, and navigation. Runtime assets are limited to the SIGAP logo, home hero, three single-bin illustrations, the location network illustration, and the device-detail illustration.

## State behavior

- Initial request: page skeleton.
- Initial failure: explicit error card with retry.
- Successful response: render data.
- Polling failure after success: retain data and show a non-destructive stale notice with retry.
- Empty list: route-specific Indonesian empty copy.
- `Offline`: show explicit badge and `last_seen`.
- `Data Invalid`: show explicit badge and reason where available.
- Nullable numeric values render `Belum tersedia` or `—`, never fabricated zero.

## Verification

The frontend must pass TypeScript checking and Expo web export. Browser smoke checks use the real Go API at 390x844 and 1366x768 for all four routes, checking navigation, API requests, asset loading, clipping, and console errors. Backend regression checks remain `go test ./... -count=1`, `go vet ./...`, and `go build -o /tmp/sigap-task03-backend ./cmd/sigap`.
