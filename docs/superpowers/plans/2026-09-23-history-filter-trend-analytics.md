# SIGAP History Filter, Trend, and Analytics Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make History filters consistently Tamagui-based and responsive, then drive KPI cards and the status trend from complete filtered analytics instead of the active table page.

**Architecture:** Extend `GET /api/history` with summary and bounded trend data computed in the same read transaction as the page. Keep draft filter state in the route, extract focused filter and analytics components to keep files below 1000 lines, and reuse current polling, motion, KPI, button, typography, and status primitives.

**Tech Stack:** Go, `database/sql`, SQLite, Expo Router, React 19, React Native Web, Tamagui 2.7.7, TypeScript 6, Vitest, ChromeDev.

**Spec:** `docs/superpowers/specs/2026-09-23-history-filter-trend-analytics-design.md`

## Global Constraints

- Work only on `feat/final-ui-ux-refinement`.
- Do not deploy, merge PR #11, force-push, or direct production traffic to this build.
- Preserve untracked `sigap-simulation.db`; never stage or remove it.
- Do not change the History table columns, row-detail semantics, cursor semantics, CSV completeness, FIS semantics, or other pages.
- Use installed Tamagui 2.7.7 and add no dependency.
- Keep every source file below 1000 lines.
- Do not add domain thresholds, status formulas, sensor semantics, or historical Offline behavior.
- Use Asia/Jakarta calendar boundaries.
- A missing period gives Trend the latest 30 local days.
- Use daily buckets through 31 days, weekly through 180 days, and monthly above 180 days.
- Return at most 120 trend buckets and report truncation.
- Respect reduced motion and do not restart animation for unchanged polling values.
- CSV uses active filters but never `limit`, `before`, or `before_id`.
- Require fresh automated and ChromeDev evidence before declaring readiness.

---

### Task 1: Add filtered summary and bounded trend queries

**Files:**
- Modify: `internal/history/history.go`
- Modify: `internal/history/history_test.go`

**Interfaces:**
- Consumes: existing `Filters`, `PageOptions`, `PageResult`, and `historyWhere`.
- Produces:

```go
const MaxTrendBuckets = 120

type Summary struct {
    Total       int `json:"total"`
    Normal      int `json:"normal"`
    Waspada     int `json:"waspada"`
    Pickup      int `json:"pickup"`
    SafetyAlert int `json:"safety_alert"`
    DataInvalid int `json:"data_invalid"`
}

type TrendBucket struct {
    Start       string `json:"start"`
    Label       string `json:"label"`
    Total       int    `json:"total"`
    Normal      int    `json:"normal"`
    Waspada     int    `json:"waspada"`
    Pickup      int    `json:"pickup"`
    SafetyAlert int    `json:"safety_alert"`
    DataInvalid int    `json:"data_invalid"`
}

type Trend struct {
    ScopeFrom    string        `json:"scope_from"`
    ScopeTo      string        `json:"scope_to"`
    Granularity string        `json:"granularity"`
    DefaultWindow bool        `json:"default_window"`
    Truncated    bool          `json:"truncated"`
    Buckets      []TrendBucket `json:"buckets"`
}

type ViewResult struct {
    Page PageResult
    Summary Summary
    Trend Trend
}

func QueryView(ctx context.Context, db *sql.DB, filters Filters, page PageOptions, now time.Time) (ViewResult, error)
```

- [ ] **Step 1: Write the failing summary test**

Create five fixture decisions covering Normal, Waspada, Angkat, Data Invalid, and an Angkat decision overridden by Safety Alert. Assert page sizes 2 and 5 return the same complete summary:

```go
want := Summary{Total: 5, Normal: 1, Waspada: 1, Pickup: 1, SafetyAlert: 1, DataInvalid: 1}
first, err := QueryView(ctx, db, Filters{}, PageOptions{Limit: 2}, fixedNow)
if err != nil { t.Fatal(err) }
second, err := QueryView(ctx, db, Filters{}, PageOptions{Limit: 5}, fixedNow)
if err != nil { t.Fatal(err) }
if first.Summary != want || second.Summary != want { t.Fatalf("first=%+v second=%+v", first.Summary, second.Summary) }
if first.Page.TotalCount != want.Total || second.Page.TotalCount != want.Total { t.Fatal("page total and summary total differ") }
```

- [ ] **Step 2: Run the summary test and confirm RED**

Run: `go test ./internal/history -run TestQueryViewSummaryIgnoresPageAndUsesEffectiveStatus -count=1`

Expected: FAIL because `QueryView` and `Summary` do not exist.

- [ ] **Step 3: Write failing trend boundary tests**

Use a fixed clock and table cases for 31 days as `day`, 32 and 180 days as `week`, and 181 days as `month`. Add tests for a 30-day default window, Monday week starts, local-midnight assignment, missing zero buckets, Safety Alert precedence, and the 120-bucket cap.

```go
cases := []struct{ name, from, to, want string }{
    {"31 days", "2026-08-01T17:00:00Z", "2026-09-01T17:00:00Z", "day"},
    {"32 days", "2026-07-31T17:00:00Z", "2026-09-01T17:00:00Z", "week"},
    {"180 days", "2026-03-05T17:00:00Z", "2026-09-01T17:00:00Z", "week"},
    {"181 days", "2026-03-04T17:00:00Z", "2026-09-01T17:00:00Z", "month"},
}
```

- [ ] **Step 4: Run trend tests and confirm RED**

Run: `go test ./internal/history -run 'TestQueryViewTrend|TestTrend' -count=1`

Expected: FAIL because trend aggregation does not exist.

- [ ] **Step 5: Centralize effective status SQL**

Add one constant and reuse it in filtering, summary, and trend:

```go
const effectiveStatusSQL = `CASE
    WHEN dec.safety_state = 'Safety Alert' THEN 'Safety Alert'
    ELSE dec.fis_status
END`
```

- [ ] **Step 6: Implement one read-snapshot query flow**

Add a local `queryRunner` interface implemented by `*sql.DB` and `*sql.Tx`. Refactor record and count helpers to accept it. Implement `QueryView` with `db.BeginTx(ctx, &sql.TxOptions{ReadOnly: true})`, page item query, conditional summary query, trend query, then commit. Set `PageResult.TotalCount` from `Summary.Total`.

- [ ] **Step 7: Implement Asia/Jakarta trend aggregation**

Use `time.FixedZone("Asia/Jakarta", 7*60*60)`. Calculate a half-open local range, select day, Monday-based week, or month granularity, aggregate effective statuses in SQL, and fill absent buckets in Go. When more than 120 buckets are requested, return the latest 120 and set `Truncated`.

- [ ] **Step 8: Run package tests and confirm GREEN**

Run: `go test ./internal/history -count=1`

Expected: PASS.

- [ ] **Step 9: Commit**

```bash
git add internal/history/history.go internal/history/history_test.go
git commit -m "feat: aggregate filtered history analytics"
```

---

### Task 2: Expose analytics in the History API without changing CSV

**Files:**
- Modify: `internal/http/history_api.go`
- Modify: `internal/http/history_api_test.go`
- Modify: `internal/e2e/release_path_test.go`

**Interfaces:**
- Consumes: `history.QueryView`, `history.Summary`, and `history.Trend`.
- Produces: additive `summary` and `trend` fields on `GET /api/history`.
- Preserves: `GET /api/export.csv` still calls complete `history.Query` with filters only.

- [ ] **Step 1: Add failing API response assertions**

Decode:

```go
Summary history.Summary `json:"summary"`
Trend history.Trend `json:"trend"`
```

Assert `Summary.Total == Page.TotalCount`, filtered Safety Alert counts are correct, and no-period Trend has 30 daily buckets with `DefaultWindow == true`.

- [ ] **Step 2: Add a failing cursor-invariance test**

Request page 1 and page 2 using the returned cursor. Assert items differ while `summary` and `trend` remain deeply equal.

- [ ] **Step 3: Run tests and confirm RED**

Run: `go test ./internal/http -run 'TestReadAPIHistory' -count=1`

Expected: FAIL because the response omits analytics.

- [ ] **Step 4: Extend the handler response**

```go
type historyResponse struct {
    Items []history.Record `json:"items"`
    Filters history.FilterState `json:"filters"`
    Page historyPageState `json:"page"`
    Summary history.Summary `json:"summary"`
    Trend history.Trend `json:"trend"`
}
```

Call `history.QueryView(r.Context(), api.db, filters, page, api.now())`. Map its Page, Summary, and Trend into one response. Leave `ExportCSV` unchanged.

- [ ] **Step 5: Update release-path assertions**

Require valid `summary`, `trend`, and a bounded bucket array while preserving existing item and secret-leak checks.

- [ ] **Step 6: Run HTTP and E2E tests**

Run: `go test ./internal/http ./internal/e2e -count=1`

Expected: PASS.

- [ ] **Step 7: Commit**

```bash
git add internal/http/history_api.go internal/http/history_api_test.go internal/e2e/release_path_test.go
git commit -m "feat: expose filtered history analytics"
```

---

### Task 3: Add TypeScript analytics types and Jakarta date validation

**Files:**
- Modify: `lib/api/types.ts`
- Modify: `lib/api/client.test.ts`
- Modify: `app/history/index.tsx`
- Modify: `tests/history/history.test.ts`

**Interfaces:**
- Produces `HistorySummary`, `HistoryTrendBucket`, `HistoryTrend`, `validateHistoryPeriod`, and corrected `dateInputToUtcRange`.
- Keeps the route-local `HistoryFilterValues` temporarily. Task 4 replaces it with the API `HistoryFilters` type while extracting the UI draft type, avoiding an import cycle between the route and component.

- [ ] **Step 1: Add response interfaces**

```ts
export interface HistorySummary {
  total: number;
  normal: number;
  waspada: number;
  pickup: number;
  safety_alert: number;
  data_invalid: number;
}

export interface HistoryTrendBucket {
  start: string;
  label: string;
  total: number;
  normal: number;
  waspada: number;
  pickup: number;
  safety_alert: number;
  data_invalid: number;
}

export interface HistoryTrend {
  scope_from: string;
  scope_to: string;
  granularity: 'day' | 'week' | 'month';
  default_window: boolean;
  truncated: boolean;
  buckets: HistoryTrendBucket[];
}
```

Extend `HistoryResponse` with `summary` and `trend`.

- [ ] **Step 2: Add failing client and date tests**

Every History response fixture gains zero summary and an empty valid trend. Change the date expectation to:

```ts
expect(dateInputToUtcRange('2026-09-10', '2026-09-12')).toEqual({
  from: '2026-09-09T17:00:00.000Z',
  to: '2026-09-12T17:00:00.000Z',
});
expect(validateHistoryPeriod('', '')).toBeNull();
expect(validateHistoryPeriod('2026-09-10', '')).toBe('Lengkapi tanggal awal dan akhir.');
expect(validateHistoryPeriod('2026-09-12', '2026-09-10')).toBe('Tanggal awal tidak boleh melewati tanggal akhir.');
```

- [ ] **Step 3: Run focused tests and confirm RED**

Run: `npm test -- --run tests/history/history.test.ts lib/api/client.test.ts`

Expected: FAIL on conversion, validation, and response fields.

- [ ] **Step 4: Implement deterministic conversion and validation**

```ts
const JAKARTA_OFFSET_MS = 7 * 60 * 60 * 1000;

function jakartaDateBoundary(value: string, addDays: number): string {
  const [year, month, day] = value.split('-').map(Number);
  return new Date(Date.UTC(year, month - 1, day + addDays) - JAKARTA_OFFSET_MS).toISOString();
}

export function validateHistoryPeriod(fromDate: string, toDate: string): string | null {
  if (!fromDate && !toDate) return null;
  if (!fromDate || !toDate) return 'Lengkapi tanggal awal dan akhir.';
  if (fromDate > toDate) return 'Tanggal awal tidak boleh melewati tanggal akhir.';
  return null;
}
```

`dateInputToUtcRange` calls the boundary helper only for a complete valid range.

- [ ] **Step 5: Run tests and typecheck**

```bash
npm test -- --run tests/history/history.test.ts lib/api/client.test.ts
npm run typecheck
```

Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add lib/api/types.ts lib/api/client.test.ts app/history/index.tsx tests/history/history.test.ts
git commit -m "feat: type history analytics contract"
```

---

### Task 4: Replace mixed filter controls with responsive Tamagui filters

**Files:**
- Create: `components/history/HistoryFilters.tsx`
- Modify: `app/history/index.tsx`
- Modify: `tests/history/history.test.ts`

**Interfaces:**
- Produces:

```ts
export interface HistoryFilterDraft extends HistoryFilterValues {
  fromDate: string;
  toDate: string;
}

export interface HistoryFiltersProps {
  draft: HistoryFilterDraft;
  compact: boolean;
  periodError: string | null;
  onChange: (patch: Partial<HistoryFilterDraft>) => void;
  onApply: () => void;
  onReset: () => void;
  onExport: () => void;
}
```

Inside the component, alias the API type instead of importing from the route:

```ts
import type { HistoryFilters as HistoryFilterValues } from '../../lib/api/types';
```

The route imports `HistoryFilterDraft` from the component and uses API `HistoryFilters` for committed requests. The component must never import `app/history/index.tsx`.

- [ ] **Step 1: Add failing pure transition tests**

Export `appliedHistoryDraft(draft)` from the route. Assert incomplete periods return an error and no filters, valid periods emit Jakarta bounds, and device queries are trimmed.

- [ ] **Step 2: Run tests and confirm RED**

Run: `npm test -- --run tests/history/history.test.ts`

Expected: FAIL because the transition helper does not exist.

- [ ] **Step 3: Extract current filter presentation**

Move `LocationFilter`, `FilterFields`, and `Filters` into `components/history/HistoryFilters.tsx`. Keep the route as state coordinator. Remove React Native `Modal`, `Pressable`, and mobile `ScrollView` imports from the route.

- [ ] **Step 4: Implement Tamagui Sheet and touch Select adaptation**

Use `Sheet`, `Sheet.Overlay`, `Sheet.Frame`, `Sheet.Handle`, Tamagui `ScrollView`, and Select `Adapt` with `Adapt.Contents`. Confirm the exact Tamagui 2.7.7 API by typecheck. Do not add fallback behavior.

- [ ] **Step 5: Apply the approved responsive field contract**

- Desktop: one compact row at 1024px and above.
- Tablet: two-column controlled wrapping at 768px.
- Mobile: Filter and CSV toolbar plus scrollable Sheet at 390px.
- Minimum field heights: 44px desktop and 48px mobile.
- Mobile field value size: 16px.
- Visible associated labels for Period, Location, Device, and Status.
- Inline `periodError` with `role="alert"` and `aria-live="polite"`.

- [ ] **Step 6: Wire Apply and Reset**

Apply validates before changing `applied`. Invalid input keeps the Sheet open and issues no request. Valid input updates `applied`, clears the cursor stack and error, then closes the mobile Sheet. Reset clears draft, applied, cursor stack, and error.

- [ ] **Step 7: Run frontend gates**

```bash
npm test -- --run tests/history/history.test.ts
npm run typecheck
CI=1 npm run export:web
```

Expected: PASS.

- [ ] **Step 8: Commit**

```bash
git add components/history/HistoryFilters.tsx app/history/index.tsx tests/history/history.test.ts
git commit -m "feat: refine responsive history filters"
```

---

### Task 5: Render server-driven Trend and KPI cards

**Files:**
- Create: `components/history/HistoryAnalytics.tsx`
- Modify: `app/history/index.tsx`
- Modify: `tests/history/history.test.ts`
- Modify: `lib/ui/motion.test.ts`

**Interfaces:**
- Produces:

```ts
export interface HistoryAnalyticsProps {
  summary: HistorySummary;
  trend: HistoryTrend;
  compact: boolean;
}

export function trendBucketTotal(bucket: HistoryTrendBucket): number;
export function trendBucketAccessibilityLabel(bucket: HistoryTrendBucket): string;
```

- [ ] **Step 1: Replace item-grouping tests with failing bucket tests**

```ts
const bucket = {
  start: '2026-09-23', label: '23 Sep', total: 10,
  normal: 5, waspada: 2, pickup: 1, safety_alert: 1, data_invalid: 1,
};
expect(trendBucketTotal(bucket)).toBe(10);
expect(trendBucketAccessibilityLabel(bucket)).toContain('Normal 5');
expect(trendBucketAccessibilityLabel(bucket)).toContain('Data tidak valid 1');
```

- [ ] **Step 2: Run tests and confirm RED**

Run: `npm test -- --run tests/history/history.test.ts`

Expected: FAIL because analytics helpers do not exist.

- [ ] **Step 3: Implement the filtered KPI grid**

Render `Hasil filter`, `Normal`, `Perlu diangkat`, and `Safety Alert` from server Summary. Give the first card description `Seluruh pembacaan yang sesuai filter`. Reuse `KpiCard` for count-up and reduced-motion handling.

- [ ] **Step 4: Implement a shared-scale stacked Trend**

Use the largest `bucket.total` as the shared maximum. Render Normal, Waspada, Pickup, Safety Alert, and Data Invalid segments in approved semantic colors. Make every bucket focusable and expose exact counts through a Tamagui disclosure that works on hover, focus, click, and tap.

- [ ] **Step 5: Implement responsive chart behavior**

Desktop and tablet use a fixed chart viewport, shared y-axis labels, and restrained date ticks. Mobile uses an internal horizontal Tamagui ScrollView with the latest bucket visible. Show scope metadata, the approved empty state, and a truncation note when required.

- [ ] **Step 6: Keep animation bounded**

Use `useAnimatedNumber` for KPI values. Animate trend transform scale and opacity only. Extend motion tests so unchanged values do not start a new transition and reduced motion applies the target immediately.

- [ ] **Step 7: Remove page-derived analytics**

Delete `DailyStatusCount`, `groupHistoryByDay`, old Trend components, and `items.filter` summary logic. Render `HistoryAnalytics` from `data.summary` and `data.trend`, with Trend above KPI cards.

- [ ] **Step 8: Run frontend gates**

```bash
npm test -- --run tests/history/history.test.ts lib/ui/motion.test.ts
npm run typecheck
CI=1 npm run export:web
```

Expected: PASS.

- [ ] **Step 9: Commit**

```bash
git add components/history/HistoryAnalytics.tsx app/history/index.tsx tests/history/history.test.ts lib/ui/motion.test.ts
git commit -m "feat: render filtered history analytics"
```

---

### Task 6: Prove bounds and document the additive contract

**Files:**
- Modify: `internal/history/history_test.go`
- Modify: `README.md`

- [ ] **Step 1: Add an opt-in deterministic stress test**

Generate 100,000 readings in a disposable database only when `SIGAP_HISTORY_STRESS=1`. Call `QueryView` with limit 50 and assert 50 items, complete Summary, and no more than 120 Trend buckets.

```go
if os.Getenv("SIGAP_HISTORY_STRESS") != "1" { t.Skip("set SIGAP_HISTORY_STRESS=1") }
if len(result.Page.Items) != 50 || len(result.Trend.Buckets) > MaxTrendBuckets {
    t.Fatalf("items=%d buckets=%d", len(result.Page.Items), len(result.Trend.Buckets))
}
```

- [ ] **Step 2: Run stress and query-plan checks**

Run: `SIGAP_HISTORY_STRESS=1 go test ./internal/history -run TestHistoryAnalyticsStress -count=1 -v`

Inspect page, summary, and trend query shapes with `EXPLAIN QUERY PLAN`. Add an index migration only if this evidence identifies a specific avoidable scan, then cover that migration in `internal/database/database_test.go`.

- [ ] **Step 3: Update README**

Document that History returns a cursor page, complete filtered Summary, and bounded Asia/Jakarta Trend. Document the 30-day default window, granularity field, default-window flag, truncation flag, and unchanged complete CSV behavior. Do not include invented domain values.

- [ ] **Step 4: Run checks**

```bash
go test ./internal/history ./internal/database -count=1
git diff --check
```

Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add internal/history/history_test.go README.md
git commit -m "test: cover bounded history analytics"
```

---

### Task 7: Verify the exact local build and prepare a non-production handoff

**Files:**
- Modify only when verification finds a defect within the files listed above.
- Store screenshots and disposable databases only under `/tmp/sigap-history-analytics-*`.

- [ ] **Step 1: Run frontend gates**

```bash
npm test -- --run
npm run typecheck
CI=1 npm run export:web
git diff --check
```

Expected: PASS.

- [ ] **Step 2: Run backend gates**

```bash
go test ./... -count=1
go vet ./...
go test -race ./... -count=1
```

Expected: PASS.

- [ ] **Step 3: Build an isolated release artifact**

Run: `SIGAP_RELEASE_OUTPUT=/tmp/sigap-history-analytics-release ./scripts/build-release.sh`

Expected: PASS with output outside the repository.

- [ ] **Step 4: Start only the exact local simulation build**

Stop the old local runtime and start the new artifact against a disposable database under `/tmp`. `GET http://localhost:8080/api/runtime` must return `{"simulation":true}`. Do not update any production service or external environment.

- [ ] **Step 5: Seed varied synthetic periods**

Use disposable data spanning daily, weekly, monthly, empty-bucket, Waspada, Angkat, Safety Alert, and Data Invalid cases. Verify API Summary and Trend before browser inspection.

- [ ] **Step 6: Verify desktop with ChromeDev**

At 1920x1080 and 1366x768 verify compact fields, Select affordances, period validation, Trend above KPI, shared chart scale, exact bucket disclosure, full-filter KPI values, pagination invariance, and no overflow. Save `/tmp/sigap-history-analytics-1920.png` and `/tmp/sigap-history-analytics-1366.png`.

- [ ] **Step 7: Verify tablet with ChromeDev**

At 768x1024 verify controlled wrapping, chart readability, two-column KPI layout, page-size controls, and vertical scrolling. Save `/tmp/sigap-history-analytics-768.png`.

- [ ] **Step 8: Verify mobile with ChromeDev**

At 390x844 verify filter Sheet, adapted Selects, Sheet scrolling, period validation, internal Trend scrolling, two-column KPI layout, bottom-navigation clearance, and no page overflow. Save `/tmp/sigap-history-analytics-390.png`.

- [ ] **Step 9: Verify Network and accessibility**

Record that draft edits send no request; Apply and Reset each send one intended request and reset cursor; page sizes send limits 10, 20, and 50; polling does not overlap; CSV excludes pagination. Keyboard-test every field and trend bucket, visible focus, labels, Sheet close behavior, and touch targets. Confirm zero material console errors.

- [ ] **Step 10: Inspect final Git state**

```bash
git status --short --branch
git log --oneline -10
git diff origin/feat/final-ui-ux-refinement...HEAD --stat
```

Confirm only approved files are tracked and `sigap-simulation.db` remains untracked.

- [ ] **Step 11: Commit only a task-scoped verification fix if needed**

Stage explicit paths and use `git commit -m "fix: complete history analytics verification"`. Do not push, deploy, or merge until every completed gate and remaining risk is reported.
