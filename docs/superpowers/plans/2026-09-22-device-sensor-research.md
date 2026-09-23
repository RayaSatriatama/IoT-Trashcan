# Device Sensor Research Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: use subagent-driven-development to execute this plan task by task with scoped review.

**Goal:** Implement a truthful, responsive Device Detail inspection view with aggregate decision summary, richer slot comparison, and per-sensor research evidence.

**Architecture:** Extend the existing ingest and research contracts additively. Persist detailed decision evidence as JSON next to current decision rows, project it through the existing read API, and render it with the current Tamagui design system. Preserve legacy records and isolate simulation-only domain values.

**Tech Stack:** Go, SQLite migrations, TypeScript, React Native Web, Expo Router, Tamagui, Vitest, Go testing.

**Spec:** `docs/superpowers/specs/2026-09-22-device-sensor-research-design.md`

## Global Constraints

- Stay on `feat/final-ui-ux-refinement`; do not merge PR #11 or force-push.
- Preserve untracked `sigap-simulation.db`.
- Do not invent production thresholds, calibration, ppm, or missing legacy sensor evidence.
- Keep FIS inputs exactly Fill, Weight, OGI, and Accumulation Time.
- Keep B3 safety inputs outside collection FIS.
- Use existing motion primitives and reduced-motion support.
- Keep every source file below 1000 lines.
- Keep the latest verified simulation build running after completion.

## Task 1: Define and validate the additive ingest contract

**Files:**
- Modify: `internal/ingest/model.go`
- Modify: `internal/ingest/validation.go`
- Test: `internal/ingest/pipeline_test.go`

1. Add failing table-driven tests proving legacy slots remain valid, documented sensor states are accepted, unknown states are rejected, and an MQ engineering unit of `ppm` requires both engineering value and calibration version.
2. Run the focused Go tests and confirm the new cases fail for the expected missing contract.
3. Add optional OGI, accumulation, and sensor records to the ingest model with strong sensor-state constants.
4. Implement minimal validation without deriving or defaulting missing values.
5. Re-run focused tests and refactor duplicated validation only after green.

## Task 2: Produce and persist auditable FIS and B3 safety evidence

**Files:**
- Modify: `internal/fuzzy/model.go`
- Modify: `internal/fuzzy/engine.go`
- Modify: `internal/fuzzy/engine_test.go`
- Modify: `internal/ingest/service.go`
- Modify: `internal/ingest/service_test.go`
- Modify: `internal/ingest/safety_integration_test.go`
- Modify: `internal/safety/safety.go`
- Modify: `internal/safety/safety_test.go`
- Create: `migrations/004_device_research_trace.sql`

1. Add failing Fuzzy tests that assert per-input membership degrees are returned and missing one of the required profile inputs cannot silently become a valid result.
2. Add failing service tests for per-stream profile selection, trace persistence, B3-only safety evaluation, sensor invalidation, and Safety Alert precedence.
3. Extend Fuzzy results with membership evidence while preserving score/status/version behavior.
4. Add optional per-stream engines to the ingest service, retaining the existing single-engine constructor path for compatibility.
5. Persist trace and safety JSON in nullable decision columns using the migration.
6. Build B3 safety input only from explicit MQ-7, MQ-8, and DS18B20 sensor records. Never reuse MQ-135 as a safety shortcut.
7. Re-run focused packages and ensure legacy tests remain green.

## Task 3: Emit truthful multi-sensor simulation data

**Files:**
- Modify: `internal/simulator/simulator.go`
- Modify: `internal/simulator/simulator_test.go`
- Modify: `internal/simulator/scenarios.go`
- Modify: `cmd/sigap/main.go`

1. Add failing simulator tests for exact per-stream sensor inventory, raw/index MQ units, no uncalibrated ppm, four derived collection inputs, and representative sensor states.
2. Define simulation-only per-stream four-input FIS profiles and B3 safety configuration.
3. Emit physical sensor records, OGI, and accumulation from the simulator data source.
4. Wire per-stream engines only when simulation mode is active.
5. Re-run simulator, command, and ingest integration tests.

## Task 4: Project research evidence through the read API and frontend types

**Files:**
- Modify: `internal/http/read_api.go`
- Modify: `internal/http/read_api_test.go`
- Modify: `lib/api/types.ts`
- Modify: `lib/api/client.test.ts`

1. Add failing API tests for sensor projection, derived values, FIS trace, B3 safety, aggregate decision summary, and legacy unavailable values.
2. Parse the original slot payload and persisted decision JSON without synthesizing absent fields.
3. Add the backward-compatible research response fields and compute a deterministic summary using effective severity then score.
4. Extend frontend DTO types to exactly mirror the response.
5. Re-run focused Go and TypeScript API tests.

## Task 5: Implement the responsive Device Detail UI

**Files:**
- Modify: `app/devices/[id].tsx`
- Modify: `components/SlotCard.tsx`
- Modify: `components/ResearchPanel.tsx`
- Create: `components/DeviceDecisionSummary.tsx`
- Create: `components/SensorStateBadge.tsx`
- Create: `components/FuzzyTrace.tsx`
- Modify: `tests/routes/detail.test.ts`
- Modify: `tests/research/research-panel.test.ts`
- Modify: `lib/ui/slotPresentation.ts`
- Modify: `lib/ui/slotPresentation.test.ts`

1. Add failing helper and source-projection tests for summary formatting, physical-versus-derived rows, membership labels, sensor-state labels, non-ppm MQ rendering, and B3 safety separation.
2. Load research data with bounded polling from page mount so the operational view never depends on opening a disclosure.
3. Add the decision summary to the hero using only API evidence.
4. Enrich each slot card with FIS result, score, memberships, sensor metadata, and existing bounded metric animations.
5. Add a separate B3 safety panel whose alert presentation overrides the ordinary collection presentation without recoloring the whole card.
6. Replace the generic research disclosure with per-slot physical sensor groups, derived parameters, and Fuzzy traces.
7. Render semantic desktop tables and mobile sensor cards at existing breakpoints with no horizontal overflow.
8. Re-run focused frontend tests, typecheck, and web export.

## Task 6: Documentation, independent review, runtime QA, and release proof

**Files:**
- Modify: `README.md`
- Create: `design-qa.md`

1. Document the additive sensor/research contract, simulation-only nature of profiles, and no-uncalibrated-ppm rule.
2. Run an independent spec and code-quality review over the complete diff; resolve material findings and re-review the fixes.
3. Build and start a fresh simulation binary from the exact final HEAD.
4. Use Chrome DevTools at 360x800, 390x844, 768x1024, 1024x768, 1366x768, and 1440x900. Verify layout, scroll reachability, no horizontal overflow, per-sensor evidence, Fuzzy/safety separation, animation, reduced-motion behavior, console, and network.
5. Add desktop and mobile screenshot paths and comparison notes to `design-qa.md`.
6. Run `npm test -- --run`, `npm run typecheck`, `CI=1 npm run export:web`, `go test ./... -count=1`, `go vet ./...`, `go test -race ./... -count=1`, the release build, and `git diff --check`.
7. Commit focused changes, push to the existing branch, verify the remote SHA, and leave the latest simulation build active.
