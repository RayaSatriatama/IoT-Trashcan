# SIGAP UI Fidelity QA

## Reference set

Approved visual references:

- `docs/SIGAP_Asset_Pack_Complete_v0.6/01_ui_pages/ui_home_desktop_mobile.png` (1448x1086)
- `docs/SIGAP_Asset_Pack_Complete_v0.6/01_ui_pages/ui_location_device_desktop_mobile.png` (1448x1086)
- `docs/SIGAP_Asset_Pack_Complete_v0.6/01_ui_pages/ui_device_detail_desktop_mobile.png` (1448x1086)
- `docs/SIGAP_Asset_Pack_Complete_v0.6/01_ui_pages/ui_history_report_desktop_mobile.png` (1448x1086)

The references were used as composition and styling targets. No reference image is rendered as a page background.

## Implementation evidence

ChromeDev screenshots from the rebuilt integrated Go binary:

| Page | Desktop | Mobile | Reference | Correction evidence |
| --- | --- | --- | --- | --- |
| Home | `/tmp/ui-final2-home-1366.png` | `/tmp/ui-final2-home-390.png` | `ui_home_desktop_mobile.png` | Semantic KPI icons, compact simulation badge, compact hero, location artwork |
| Locations | `/tmp/ui-final2-locations-1366.png` | `/tmp/ui-final2-locations-390.png` | `ui_location_device_desktop_mobile.png` | Location thumbnails, shared KPI cards, compact search and scrollable status chips |
| Location detail | `/tmp/ui-final2-location-detail-1366.png` | `/tmp/ui-final2-location-detail-390.png` | `ui_location_device_desktop_mobile.png` | Location artwork, compact device heading, three-bin device cards |
| Device detail | `/tmp/ui-final2-device-1366.png` | `/tmp/ui-final2-device-390.png` | `ui_device_detail_desktop_mobile.png` | Three-bin hero, slot metric hierarchy, explicit unavailable values |
| History | `/tmp/ui-final2-history-1366.png` | `/tmp/ui-final2-history-390.png` | `ui_history_report_desktop_mobile.png` | Shared KPI cards, daily stacked trend, dense desktop history rows |

All screenshots were captured after opening the matching reference in ChromeDev, then reloading the actual integrated runtime at 1366x768 and 390x844. No reference image is rendered as a page background.

The runtime used real simulation API data from the embedded SPA binary. The simulation banner, status badges, empty/null handling, research disclosure, and navigation were verified in the rendered UI.

## Viewport checks

| Viewport | Page checked | Result |
| --- | --- | --- |
| 360x800 | History | No horizontal overflow; stacked controls remain usable |
| 390x844 | Home | No horizontal overflow; mobile bottom navigation visible |
| 768x1024 | History | No horizontal overflow; responsive card layout retained |
| 1024x768 | History | No horizontal overflow; desktop navigation boundary retained |
| 1366x768 | Home, Locations, Location Detail, Device Detail, History | No horizontal overflow; desktop composition and assets visible |
| 1440x900 | History | No horizontal overflow; content remains within the application shell |

## Focused visual findings

- Navy heading hierarchy, pale application background, white cards, green primary actions, and semantic status surfaces align with the approved direction.
- Desktop sidebar and mobile bottom navigation remain distinct responsive patterns.
- Approved runtime illustrations are used for the hero, location, device, slot, and history surfaces.
- KPI cards remain limited to four on Home.
- Device detail uses three slot cards and a collapsed-by-default research disclosure.
- Missing metrics render as `Belum tersedia`; raw `gas_raw` remains explicitly raw.
- Location cards use the approved `05_location_assets` artwork through a static Metro-safe mapping.
- Sidebar, bottom navigation, KPI cards, filters, back actions, and activity rows use the same `lucide-react-native` icon family.
- The history page includes a pure React Native stacked daily trend without a chart dependency, while desktop records use compact table-like rows.
- The simulation indicator is a 30 to 36 pixel contextual pill instead of a full-width warning bar.

## Functional checks

- Browser console: no messages reported during the final route checks.
- Same-origin API requests were served by the integrated Go binary.
- `/`, `/locations`, `/locations/sim-location-main`, `/devices/sim-device-lobby`, and `/history` rendered successfully.
- The expanded research panel loaded raw, normalized, decision, and sensor-health sections.
- Final Home network capture returned 200 for the SPA document, bundle, runtime metadata, overview API, and runtime artwork with no failed request.

## Iteration history

1. Replaced serif browser fallback with the configured sans-serif body font.
2. Added responsive hero, KPI, status, filter, location, device, slot, and history compositions.
3. Added approved artwork to runtime surfaces and corrected mobile two-column KPI behavior.
4. Rounded near-integer research metrics to avoid floating-point presentation artifacts.
5. Added shared semantic icon and KPI components, approved location artwork mapping, compact simulation indicator, and daily history trend.
6. Rebuilt and rechecked the embedded SPA after the mobile chip and location-detail heading corrections.

## Result

passed for the visual correction scope

## Readability and scroll pass

The shell now uses one vertical `ScrollView` for the content column. This is required because React Native Web keeps the document body at `overflow: hidden`; the scroll surface now owns the page content while the desktop sidebar and mobile bottom navigation remain stable.

ChromeDev evidence:

- `/tmp/readability-home-1366.png`, `/tmp/readability-home-390.png`
- `/tmp/readability-locations-1366.png`, `/tmp/readability-final-locations-390.png`
- `/tmp/readability-location-detail-1366.png`, `/tmp/readability-location-detail-390.png`
- `/tmp/readability-device-1366.png`, `/tmp/readability-device-390.png`
- `/tmp/readability-history-1366.png`, `/tmp/readability-history-390.png`

At 390x844, all five routes reached the bottom of their scroll surface with no horizontal overflow. The same six-viewport audit covered 360x800, 390x844, 768x1024, 1024x768, 1366x768, and 1440x900. Locations correctly remains non-scrollable at wide viewports when all content fits within the viewport.

Typography now uses named styles in `lib/ui/typography.ts`, larger body and caption tokens, `$body` font binding for numeric and metadata styles, and stronger readable secondary text. The final ChromeDev route check reported no console messages and API/assets returned 200.

# Device Detail Design QA

## Source of truth

- Reference: screenshot Device Detail supplied in the implementation request.
- Implementation route: `/devices/sim-device-canteen`.
- Runtime: release binary built from the current `feat/final-ui-ux-refinement` working tree with simulation enabled.

## Evidence

- Desktop 1366 x 768: `/tmp/sigap-device-final-1366.png`
- Mobile 390 x 844: `/tmp/sigap-device-final-390.png`
- Mobile final-scroll state: `/tmp/sigap-device-final-390-bottom.png`

## Comparison

The implementation preserves the reference hierarchy: device identity and decision summary, three stream cards, then per-slot research evidence. The rendered values come from the research API rather than screenshot fixtures. Organic, inorganic, and B3 retain independent stream colors, while operational status remains encoded by status components. B3 collection inputs remain separate from its safety channels.

Desktop uses three equal-height slot cards at 1366 and 1440. At 1024 the three cards remain aligned in one row. At 768 they reorganize to two plus one, and at 360 and 390 they stack into one column. The research area uses three columns where space permits and sensor cards without horizontal scrolling on mobile.

## Findings and corrections

- P0: none.
- P1: none.
- P2: none after correction. The first preview exposed unequal card heights, UTC-only update time, excessive numeric precision, and misleading `Norm.` labels for FIS inputs. These were corrected before final evidence.
- P3: simulation FIS reasons remain English because they are runtime decision evidence and were not rewritten in presentation code.

## Final result

passed
