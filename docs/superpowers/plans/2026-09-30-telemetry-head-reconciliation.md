# SIGAP Telemetry HEAD Reconciliation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Align the firmware PRD/TSD and migration regression coverage with the current `main` integration state without changing runtime behavior.

**Architecture:** Keep `6fed00f` as historical evidence and document `ad65d5f` as the current reconciliation target. Treat the existing summary ingest, research trace, safety evidence, and history APIs as compatibility surfaces; add only documentation and a regression assertion for already-applied migration `004`.

**Tech Stack:** Markdown, Go `database/sql` tests, SQLite migrations, DOCX Open XML, Git worktree.

**Spec:** `docs/superpowers/specs/2026-09-30-telemetry-head-reconciliation-design.md`

## Global Constraints

- Do not revert, rebase, or amend commits already merged to `main`.
- Do not modify `POST /api/ingest`, existing dashboard routes, simulator behavior, or current SQLite data.
- The current expected telemetry migration name is `005_telemetry_archive.sql`; recheck the migration directory immediately before implementation.
- Preserve `trace_json` and `safety_json` added by `004_device_research_trace.sql`.
- Keep `6fed00f` as historical baseline and `ad65d5f` as current reconciliation target.
- Do not add PlatformIO firmware, a telemetry handler, or telemetry tables in this reconciliation change.

## Review Focus

- Historical baseline versus current HEAD: both hashes must appear with the correct roles in PRD and TSD.
- Migration order: the test must assert `004_device_research_trace.sql` and both decision columns.
- Route compatibility: archive documentation must distinguish new batch transport from existing `POST /api/ingest`.
- Research trace preservation: documentation must not imply that archive records replace `trace_json` or `safety_json`.
- DOCX parity: the binary PRD must contain the same baseline, target, migration, and route-separation decisions as Markdown.

---

### Task 1: Update firmware reconciliation documentation

**Files:**
- Modify: `docs/Microcontroller/TSD_SIGAP_ESP32S3_PlatformIO.md` at the repository compatibility section, target tree, ingest section, and implementation sequence.
- Modify: `docs/Microcontroller/PRD_Firmware_SIGAP_ESP32S3_PlatformIO.md` at the repository baseline section, research/export section, and developer references.
- Test: shell assertions with `rg` over both Markdown files.

**Interfaces:**
- Consumes: current `internal/http/router.go`, `internal/http/ingest_handler.go`, `internal/http/read_api.go`, `internal/database/migrate.go`, `migrations/004_device_research_trace.sql`.
- Produces: explicit historical/current commit roles, additive batch-ingest rules, and the current expected next migration number.

- [ ] **Step 1: Add a dated reconciliation section to both Markdown documents.**

  State that `6fed00f02b6ef7dabc159d924f798d344a6072ba` remains the historical observation baseline and `ad65d5fe73c30b16c61da917eb517cdf2a783ba6` is the current reconciliation target. Include a compact table classifying `98e4439` as runtime/schema, `292aa47` as firmware documentation, and `ad65d5f` as tooling-only.

- [ ] **Step 2: Document compatibility boundaries.**

  In the TSD, state that `POST /api/ingest` remains summary-v2, a future batch route is additive, hardware v2/v3 records do not invoke legacy inference, and archive records do not overwrite `trace_json` or `safety_json`.

- [ ] **Step 3: Replace the migration placeholder with the current expected name plus a recheck rule.**

  Change the target tree entry to `migrations/005_telemetry_archive.sql`, then state that the implementation PR must recheck the directory and use the next free ordered number if another migration lands first.

- [ ] **Step 4: Document current history cursor compatibility.**

  Record that current dashboard history uses `(recorded_at, reading_id)`, while telemetry archive identity uses `(device_id, boot_id, sequence)`. Require additive archive DTOs and exports without silently changing current history ordering or existing CSV columns.

- [ ] **Step 5: Verify document assertions.**

  Run:

  ```bash
  rg -n '6fed00f02b6ef7dabc159d924f798d344a6072ba|ad65d5fe73c30b16c61da917eb517cdf2a783ba6|98e4439|292aa47|ad65d5f|005_telemetry_archive|POST /api/ingest|trace_json|safety_json' docs/Microcontroller/*.md
  git diff --check
  ```

  Expected: both Markdown files contain the baseline and target roles, the three commit identifiers, the `005` rule, the existing route, and both trace columns; whitespace check passes.

- [ ] **Step 6: Commit the documentation slice.**

  ```bash
  git add docs/Microcontroller/PRD_Firmware_SIGAP_ESP32S3_PlatformIO.md docs/Microcontroller/TSD_SIGAP_ESP32S3_PlatformIO.md
  git commit -m "docs: reconcile firmware specs with current head"
  ```

### Task 2: Strengthen the existing migration regression test

**Files:**
- Modify: `internal/database/database_test.go` in `TestOpenMigrationIsIdempotent`.
- Test: `internal/database/database_test.go` itself.

**Interfaces:**
- Consumes: `database.Open`, embedded migration list, and SQLite schema metadata.
- Produces: a regression assertion that migration `004_device_research_trace.sql` ran and exposed both decision evidence columns.

- [ ] **Step 1: Expand the migration-name assertion.**

  Include `004_device_research_trace.sql` in the existing `schema_migrations` query and change the expected count from `3` to `4`.

- [ ] **Step 2: Add column assertions.**

  Query `PRAGMA table_info(decisions)` and assert that exactly one row exists for each of `trace_json` and `safety_json`. Fail with the missing column name so a future migration omission is diagnosable.

- [ ] **Step 3: Run the focused test.**

  ```bash
  go test ./internal/database -count=1
  ```

  Expected: package passes and the idempotence test observes four applied migrations and both columns.

- [ ] **Step 4: Run all Go tests.**

  ```bash
  go test ./... -count=1
  ```

  Expected: all existing Go tests pass; no runtime code was changed.

- [ ] **Step 5: Commit the regression slice.**

  ```bash
  git add internal/database/database_test.go
  git commit -m "test: cover device research migration"
  ```

### Task 3: Synchronize the PRD DOCX artifact

**Files:**
- Modify: `docs/Microcontroller/PRD_Firmware_SIGAP_ESP32S3_PlatformIO.docx`.
- Create: `tools/sync_firmware_prd_docx.py` only if the existing DOCX cannot be regenerated with an available converter.
- Test: a ZIP/XML text assertion over `word/document.xml`.

**Interfaces:**
- Consumes: corrected PRD Markdown and the existing DOCX package.
- Produces: a readable DOCX whose visible text contains the same reconciliation decisions as Markdown.

- [ ] **Step 1: Detect the available converter or DOCX library.**

  Check `pandoc`, `libreoffice`, `soffice`, and `python-docx`. If one is available, use it in a temporary output directory; otherwise create the small deterministic `tools/sync_firmware_prd_docx.py` utility with `apply_patch` and Python standard-library `zipfile` plus XML handling.

- [ ] **Step 2: Regenerate the DOCX from the corrected PRD content.**

  Preserve the existing document package and styles. Ensure the visible document text contains the historical baseline, current target, three commit classifications, `005_telemetry_archive.sql`, additive `POST /api/ingest` rule, and `trace_json`/`safety_json` preservation rule.

- [ ] **Step 3: Verify DOCX parity.**

  ```bash
  python3 - <<'PY'
  from pathlib import Path
  from zipfile import ZipFile
  text = ZipFile(Path('docs/Microcontroller/PRD_Firmware_SIGAP_ESP32S3_PlatformIO.docx')).read('word/document.xml').decode()
  for value in ('6fed00f', 'ad65d5f', '98e4439', '292aa47', '005_telemetry_archive.sql', 'POST /api/ingest', 'trace_json', 'safety_json'):
      assert value in text, value
  PY
  ```

  Expected: all required decisions are present in the DOCX XML and the package opens as a valid ZIP.

- [ ] **Step 4: Commit the artifact slice.**

  ```bash
  git add docs/Microcontroller/PRD_Firmware_SIGAP_ESP32S3_PlatformIO.docx tools/sync_firmware_prd_docx.py
  git commit -m "docs: synchronize firmware PRD document"
  ```

### Task 4: Final verification and handoff

**Files:**
- Verify: all files changed by Tasks 1-3.

**Interfaces:**
- Consumes: the three reconciliation commits and current `main`.
- Produces: a reviewable branch and pull request with no runtime behavior change.

- [ ] **Step 1: Inspect the complete diff.**

  ```bash
  git diff origin/main...HEAD --stat
  git diff origin/main...HEAD --check
  git status --short --branch
  ```

  Expected: only the two Markdown docs, the migration test, the PRD DOCX, and the optional deterministic utility are changed.

- [ ] **Step 2: Run the complete project verification.**

  ```bash
  go test ./... -count=1
  npm test -- --run
  npm run typecheck
  npm run build
  ```

  Expected: all checks pass; no Vercel runtime route or API behavior changes are introduced.

- [ ] **Step 3: Commit or push only after review.**

  Open a PR from `docs/telemetry-reconciliation-spec` to `main` with the spec, plan, documentation, regression test, and DOCX artifact. Do not include the local database, `.agents` working-tree copies, screenshots archives, or unrelated dirty files.
