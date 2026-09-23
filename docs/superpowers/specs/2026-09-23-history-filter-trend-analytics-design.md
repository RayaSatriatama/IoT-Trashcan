# SIGAP History Filter, Trend, and Analytics Design

Date: 2026-09-23

Status: Approved design, pending implementation plan

Scope: `/history` filter controls, filtered KPI summary, status trend, related History API additions, responsive behavior, animation, and verification.

## 1. Objective

The History workspace must let an operator:

1. Define a clear audit scope through consistent Tamagui controls.
2. Apply that scope explicitly without request spam.
3. Read summary counts for the complete filtered result, independent of pagination.
4. Compare status distribution over time with an accurate, bounded trend.
5. Continue using stable cursor pagination and complete filtered CSV export.

The existing History table structure, row detail, device semantics, FIS semantics, and CSV completeness remain unchanged.

## 2. Approved Decisions

- `GET /api/history` remains the single interactive History request.
- The response gains additive `summary` and `trend` fields.
- Summary counts cover the complete applied filter, not the active page.
- A missing period filter gives the trend a rolling 30-day window.
- An explicit range up to 31 days uses daily buckets.
- An explicit range from 32 through 180 days uses weekly buckets.
- An explicit range above 180 days uses monthly buckets.
- Period input is either empty or complete. A single date is rejected in the UI with an inline validation message.
- The first KPI label changes from `Ditampilkan` to `Hasil filter`.
- Safety Alert overrides the ordinary FIS status in filters, summary counts, and trend buckets.
- `Waspada` and canonical status `Data Invalid`, displayed as `Data tidak valid`, are represented in the analytics contract and trend.
- History dates use Asia/Jakarta calendar boundaries.
- Filter controls use Tamagui primitives, including touch-adapted Select sheets.
- KPI and trend animation runs only when successful data changes and respects reduced motion.
- CSV remains a complete filtered export and never inherits cursor or page-size parameters.

## 3. Non-Goals

- Redesigning the History table or row detail.
- Changing cursor pagination semantics.
- Changing CSV columns or completeness.
- Adding a chart library, form library, or state-management library.
- Defining new FIS thresholds, safety thresholds, status formulas, or sensor semantics.
- Adding historical Offline semantics. Offline remains unavailable as a History status until the domain defines how it is persisted.
- Changing any non-History page.

## 4. Current Problems

### 4.1 Filter controls

- The outer mobile filter uses a React Native modal while the fields use mixed Tamagui and platform behavior.
- Touch Select content can extend beyond the mobile viewport and cover filter actions.
- Field height, focus treatment, and select affordance are inconsistent.
- Draft date values can currently form a partial range whose analytics meaning is unclear.

### 4.2 Trend

- The current chart derives its values from `data.items`, so it describes only the active page.
- It slices the result to seven item-derived days, regardless of the applied period.
- `Waspada` and canonical status `Data Invalid` can create a day bucket but add no visible bar height.
- One active page can contain records from only one day even when the filter covers weeks or months.
- The current bars do not provide a shared, explicit comparison scale.

### 4.3 KPI cards

- KPI values derive from the active page, so changing page size or cursor changes the apparent operational totals.
- The label `Ditampilkan` implies visible rows, while the desired value is the complete filtered count.
- Polling can make current-page-derived values appear rigid or misleading when new readings arrive.

## 5. User Flow

```text
Open History
  -> view the last successful table, summary, and trend
  -> edit draft filters
  -> no request occurs
  -> apply a complete valid filter
  -> cursor resets to page 1
  -> one History request runs
  -> table, total pages, summary, and trend update from one response
  -> successful changed values animate briefly
```

Reset clears draft and applied filters, resets pagination, and issues one History request.

Changing page size or cursor changes the page projection but does not change summary or trend while applied filters remain unchanged.

## 6. Filter Component Design

### 6.1 Component boundary

```text
HistoryPage
  HistoryFilterToolbar
  HistoryFilters
    PeriodField
    LocationSelect
    DeviceSearch
    StatusSelect
    FilterActions
  HistoryMobileFilterSheet
  HistoryTrend
  HistorySummaryCards
  Existing HistoryTable or HistoryCards
```

The existing `app/history/index.tsx` remains the page coordinator. A focused History filter component may be extracted if keeping the implementation in the page would make the file approach the repository's 1000-line limit. No generic form framework will be introduced.

### 6.2 Tamagui primitives

Use existing project components and Tamagui primitives:

- `Label`
- `Input`
- `Select`
- `Adapt`
- `Sheet`
- `XStack`
- `YStack`
- `ScrollView`
- `AppButton`

Location and Status use `Select.Trigger`, `Select.Content`, and a visible chevron. On touch viewports, `Adapt` renders the options within a Tamagui `Sheet` so the options and filter actions remain reachable.

### 6.3 Field contract

- Visible label: 12 to 13px, weight 600.
- Input and Select value: 14px desktop, 16px mobile.
- Minimum height: 44px desktop, 48px mobile.
- Shared border, radius, surface, disabled state, and focus ring tokens.
- Every label is associated with its control.
- Errors appear next to the affected group and use `aria-live`.

### 6.4 Period

The period is one concept with two related date inputs:

```text
Periode
[Dari tanggal] sampai [Sampai tanggal]
```

Both values are empty or both are present. Apply is blocked when only one value is set or when the start is after the end.

Date-only values are converted using Asia/Jakarta boundaries. For 10 September through 12 September 2026:

```text
from = 2026-09-09T17:00:00Z
to   = 2026-09-12T17:00:00Z
```

`to` remains exclusive and therefore includes the full final local calendar day.

### 6.5 State and requests

```ts
type HistoryFilterState = {
  fromDate: string
  toDate: string
  locationId: string
  deviceQuery: string
  status: HistoryStatus | ''
}
```

The page owns separate `draftFilters` and `appliedFilters`.

- Field edits update only `draftFilters`.
- Apply validates, commits the draft, clears the cursor stack, and requests page 1.
- Reset clears both states, clears the cursor stack, and requests page 1.
- Polling always uses `appliedFilters`.
- Stale responses cannot replace data for a newer filter state.

## 7. Responsive Filter Layout

### 7.1 Desktop at 1024px and above

```text
Periode          Lokasi       Perangkat          Status       Reset  Terapkan
[date] [date]    [select]     [search]           [select]
```

Recommended width proportions:

- Period: 2.2fr
- Location: 1.15fr
- Device: 1.5fr
- Status: 1.2fr
- Actions: intrinsic width

CSV stays in the filter toolbar outside the form submission group.

### 7.2 Tablet at 768px

```text
Periode                         Lokasi
[date] [date]                   [select]

Perangkat                       Status
[search]                        [select]

                         [Reset] [Terapkan]
```

Fields wrap without reducing the approved typography or touch targets.

### 7.3 Mobile at 390px

The page toolbar shows only:

```text
[Filter]                         [Unduh CSV]
```

Filter opens a scrollable Tamagui Sheet containing Period, Location, Device, Status, Reset, and Apply. Draft state persists when the sheet closes. Touch Selects use their own adapted sheet behavior without clipping the viewport. The page must not gain horizontal overflow.

## 8. API Contract

The History response remains backward compatible through additive fields:

```ts
interface HistoryResponse {
  items: HistoryItem[]
  filters: HistoryAppliedFilters
  page: {
    limit: number
    has_more: boolean
    next: HistoryCursor | null
    total_count: number
  }
  summary: HistorySummary
  trend: HistoryTrend
}

interface HistorySummary {
  total: number
  normal: number
  waspada: number
  pickup: number
  safety_alert: number
  data_invalid: number
}

interface HistoryTrend {
  scope_from: string
  scope_to: string
  granularity: 'day' | 'week' | 'month'
  default_window: boolean
  truncated: boolean
  buckets: HistoryTrendBucket[]
}

interface HistoryTrendBucket {
  start: string
  label: string
  total: number
  normal: number
  waspada: number
  pickup: number
  safety_alert: number
  data_invalid: number
}
```

The example structure does not prescribe runtime values. All values come from persisted readings and decisions.

## 9. Backend Query Design

### 9.1 Snapshot consistency

The History handler executes the page query, summary query, and trend query in one read transaction. This prevents the simulator from inserting records between queries and producing mismatched totals in a single response.

### 9.2 Shared filtering

Existing `historyWhere` remains the source of shared date, location, device, and status conditions. Cursor conditions apply only to the page query. Summary and trend never receive the active cursor.

The effective status SQL expression is centralized and reused:

```sql
CASE
  WHEN dec.safety_state = 'Safety Alert' THEN 'Safety Alert'
  ELSE dec.fis_status
END
```

This preserves Safety Alert precedence without defining a new status formula.

### 9.3 Summary

Summary uses one conditional aggregate query over the complete applied filter. `summary.total` and `page.total_count` must match for the same response.

### 9.4 Trend scope

- No explicit period: local today and the preceding 29 local calendar days.
- Explicit period up to 31 days: daily buckets.
- Explicit period from 32 through 180 days: Monday-based weekly buckets.
- Explicit period above 180 days: monthly buckets.

The API fills missing buckets with zero values so gaps remain visible.

### 9.5 Bounded response

The trend response is capped at 120 buckets. If an explicit range produces more buckets, the API returns the latest 120 and sets `truncated` to true. This is a technical response-size limit, not a domain decision threshold.

### 9.6 Query count and indexing

The endpoint performs a constant number of database queries. It does not query once per location, device, status, or bucket.

Existing indexes are inspected with `EXPLAIN QUERY PLAN`. An index is added only if evidence shows an avoidable scan in the approved query shape. No speculative index is part of this design.

## 10. Trend Presentation

### 10.1 Data correctness

`HistoryTrend` consumes only `response.trend.buckets`. It never rebuilds analytics from `response.items`.

Stack order and semantic colors:

1. Normal: green.
2. Waspada: amber.
3. Perlu diangkat: orange.
4. Safety Alert: red.
5. Data tidak valid: purple. Its canonical API status remains `Data Invalid`.

All bars use one shared vertical scale based on the largest bucket total. A bucket with 20 readings is twice the height of a bucket with 10 readings.

### 10.2 Interaction

- Desktop supports hover, focus, and click.
- Mobile supports tap.
- The detail surface shows bucket range, total, and each nonzero status count.
- Interaction does not depend on hover.
- The active trend scope is visible as metadata.

### 10.3 Responsive behavior

Desktop and tablet use shared y-axis markers and restrained date labels. Mobile uses an internal horizontal chart scroller with the latest bucket initially visible. Internal scrolling must not create page-level horizontal overflow.

### 10.4 Empty state

```text
Belum ada data untuk periode dan filter ini.
Ubah periode atau filter untuk melihat tren.
```

## 11. KPI Summary

The four cards map to the server summary:

| Card | Source |
| --- | --- |
| Hasil filter | `summary.total` |
| Normal | `summary.normal` |
| Perlu diangkat | `summary.pickup` |
| Safety Alert | `summary.safety_alert` |

The first card metadata reads `Seluruh pembacaan yang sesuai filter`. `Waspada` and `Data Invalid`, displayed as `Data tidak valid`, remain available to the trend and future consumers without adding new KPI cards in this task.

Desktop uses four columns. Tablet and mobile use two columns. Label wrapping is limited to two lines, KPI numbers retain the approved scale, and card height remains consistent within each row.

## 12. Animation and Polling

### 12.1 KPI count-up

- Animate only after a successful response changes a value.
- Interpolate from the previous displayed value to the new value.
- Use a short 300 to 450ms ease-out transition.
- Do not reset to zero during polling.
- Skip animation when the value is unchanged.

### 12.2 Trend animation

- Animate bar changes with transform scale and opacity.
- Keep the transform origin at the bottom.
- Do not animate layout width, page position, or chart container height.
- Fade a newly introduced bucket briefly.

### 12.3 Reduced motion

Reduced-motion preference applies updated values immediately without count-up or bar transitions.

### 12.4 Refresh behavior

Initial load may use a bounded skeleton. Polling retains the last successful content. A failed refresh marks it stale and shows a concise update error without replacing values with zero or synthetic data.

Requests do not overlap. A newer applied filter aborts or supersedes the older request, and an older response cannot commit over newer state.

## 13. Error Handling

- Invalid or incomplete periods are handled before a network request.
- Backend filter validation continues to return HTTP 400 for malformed query parameters.
- Page, summary, and trend failures fail the History response together rather than returning internally inconsistent partial analytics.
- The frontend retains the last successful response during a transient polling failure.
- No fallback data is generated.

## 14. Performance Constraints

- Default table page size remains 10 in the UI.
- Page options remain 10, 20, and 50.
- Trend output has at most 120 buckets.
- The chart creates one bounded visual element per returned bucket.
- No third-party chart dependency is added.
- No unbounded History row fetch is introduced.
- CSV remains the only complete filtered row export path.
- Query plans are inspected before adding indexes.

## 15. Test Design

### 15.1 Backend tests

- Summary remains identical for page sizes 10, 20, and 50.
- Summary remains identical across cursor pages.
- `summary.total` equals `page.total_count` within the same response.
- Location, device, status, and period filters affect summary and trend.
- Safety Alert overrides Angkat and Waspada.
- Waspada and Data Invalid produce visible trend counts.
- Missing dates produce zero buckets.
- A missing period produces exactly 30 local daily buckets.
- Boundaries at 31, 32, 180, and 181 days select the approved granularity.
- Asia/Jakarta midnight, week, month, year, and leap-day boundaries are correct.
- Trend output is capped and reports `truncated`.
- CSV remains complete and ignores `limit`, `before`, and `before_id`.

### 15.2 Frontend tests

- Draft edits do not fetch.
- Apply commits filters, resets pagination, and fetches once.
- Reset clears filters, resets pagination, and fetches once.
- Incomplete or reversed periods show an inline error without fetching.
- KPI cards consume `summary` rather than `items`.
- Trend consumes `trend.buckets` rather than `items`.
- Changing cursor or page size does not derive new KPI or trend values from the page.
- Touch Select content uses the responsive sheet path.
- Count-up runs only when values change.
- Reduced motion bypasses transitions.
- Initial, empty, stale, and error states remain readable.

### 15.3 Synthetic data matrix

Deterministic fixtures cover 1, 7, 30, 31, 32, 90, 180, 181, 365, and 730 days. They include empty days, uneven volume, every supported historical status, Safety Alert precedence, several locations and devices, local midnight boundaries, month and year boundaries, and leap day.

A non-CI stress fixture with at least 100,000 readings verifies bounded responses and records `EXPLAIN QUERY PLAN` evidence. No arbitrary latency pass threshold is introduced without a project performance budget.

## 16. Browser and Network Verification

ChromeDev verification uses:

- 390x844
- 768x1024
- 1366x768
- 1920x1080

Verify:

- Consistent Tamagui filter fields.
- Touch Select and mobile Sheet remain on-screen.
- No page-level horizontal overflow.
- KPI cards update with filters rather than cursor movement.
- Daily, weekly, and monthly trend shapes reflect synthetic data.
- Hover, focus, click, and tap trend inspection.
- Draft edits send no History request.
- Apply and Reset each trigger one intended History request.
- Polling requests do not overlap.
- Page size changes send `limit=10`, `limit=20`, and `limit=50`.
- CSV requests include active filters but exclude pagination.
- Console has no material error.

## 17. Planned Files

- `internal/history/history.go`
- `internal/history/history_test.go`
- `internal/http/history_api.go`
- `internal/http/history_api_test.go`
- `lib/api/types.ts`
- `lib/api/client.test.ts`
- `app/history/index.tsx`
- `tests/history/history.test.ts`
- A focused History filter component only if needed to keep the page below the repository file-size limit.
- `README.md` for the additive History response contract.

## 18. Acceptance Criteria

- Every History filter field uses the approved Tamagui treatment.
- Desktop, tablet, and mobile filter layouts match their approved structure.
- Editing draft filters does not call the API.
- Apply and Reset each issue one intended request and reset pagination.
- Summary counts represent the full applied filter.
- Page size and cursor movement do not change summary or trend.
- A missing period displays the last 30 days of trend data.
- Explicit period granularity follows the 31-day and 180-day boundaries.
- Waspada and Data Invalid are visible in the trend when present.
- Safety Alert precedence remains intact.
- Trend bars use a common scale and expose exact values on demand.
- KPI and trend animation does not restart for unchanged polling data.
- Reduced motion is respected.
- CSV behavior is unchanged.
- No horizontal overflow appears at the required viewports.
- No N+1 query or unbounded chart DOM is introduced.
- Automated checks and ChromeDev verification provide fresh evidence before completion is claimed.
