# SIGAP Production Debug Lookahead Implementation Plan

> For agentic workers: REQUIRED SUB-SKILL: subagent-driven-development. Use the existing approved plan and its SDD workspace. This is a source-inspected scheduling and dispatch-preparation addendum, not a replacement plan or authorization to bypass prerequisites. Original tasks remain 5 through 10; lettered slices are implementation/test ownership boundaries, not extra product scope. Preserve the owner's no-Git-write exception to the skills' commit steps.

Goal: prepare the next work while independent agents are running, so the next authorized implementation starts from checked files, stable interfaces and ready test cases rather than repeating discovery.

Architecture: retain the approved typed debug contract, one page-owned BLE session, bounded independent sinks and authenticated history. Parallelize frontend versus firmware when both interfaces and build ownership are disjoint; serialize firmware composition and shared .pio output. Every implementer owns its tests; every whole task keeps its independent review gate.

Tech stack: existing repository dependencies and verified cached toolchains only. No installation, upgrades, alternate model/provider, new DOM testing library, or speculative transport.

Spec: docs/superpowers/specs/2026-10-21-sigap-production-debug-design.md.
Approved base plan: docs/superpowers/plans/2026-10-21-sigap-production-debug-implementation.md.
Root: /mnt/nvme0n1p4/Projects/Programming_Projects/full-integration-hil-validation.
SDD workspace: .superpowers/sdd/2026-10-21-sigap-production-debug-implementation/.
Baseline HEAD rechecked: 0257b9e5cc08b142348f85f6da184842c09ab420, feat/full-integration-hil-validation. Preserve the dirty tree.

## Current authorization takes precedence over historical plan prerequisites

- Main agent may implement and test Task 3/4 only, by the explicit owner decision. Tasks 5 onward retain delegated implementation plus independent review.
- Delegation route is gpt-6-luna / openai-codex / medium by the latest explicit owner choice. The older plan's high requirement is historical, not permission to change settings. Recheck all three config values before each dispatch; no silent fallback.
- Heartbeat is suspended by the owner for the remaining plan. Do not restart timers, GUI monitoring or cron while doing lookahead work.
- Go 1.27.1 already exists at /home/raya/.cache/sigap-toolchains/go1.27.1/bin/go; gofmt is beside it. Use these absolute paths, GOTOOLCHAIN=local and -mod=readonly. An unqualified PATH lookup is not toolchain discovery.
- No Git writes, commit/push/merge/reset/stash/cleanup; no live DB migration, server replacement, deployment, flash, reset, provisioning/NVS/SD/profile mutation or physical sensor/actuator test. Synthetic host fixtures are TEST evidence, never physical readings.
- Existing source/host approval does not authorize physical startup effects. Overall HIL stays HOLD.
- Keep text files below 1500 lines. docs/task9-verification.md is already 1649 lines: do not expand or split it without explicit permission. Its update is a named Task 9/10 documentation gate, not an excuse to silently relocate or discard existing evidence.

### Later owner clarification: isolated 5A/6A may start early

The owner subsequently chose: "Kerjakan 5A dan 6A secara paralel dan terisolasi dulu; integrasi serta penerimaan penuh tetap menunggu Task 3. Riset HCI tetap ditahan."

This is a narrow scheduling override for the two pure implementation/test slices, not a declaration that Task3 has passed. Task3B's public TS client and Task1/2/4's core/backend interfaces are stable inputs. Only new 5A session files and pure6A worker/gate/native-test files are writable, with exclusive npm versus firmware/.pio owners. No existing production caller is changed. The original full Task3 prerequisite remains binding for integration and full Task5/6 acceptance; later slices are not released by this choice. SDK/HCI research and physical/deployment permissions remain held. If the stable contracts change, these isolated slices may require rework. The latest source-bound release and exact ownership are recorded in the SDD ledger and lookahead/dispatch-index.json.

## Verified state and source findings

Task 1 contract, Task 2 core/USB and Task 4A store are accepted host/source components. Task 3A has a returned Spec PASS / Quality APPROVED review; source hashes still match. Its reviewer prose contains test line references beyond the 186-line actual file, so preserve the verdict/report but correct evidence attribution in the final whole-Task3 review rather than copy those references as verified source positions. Task 3B implementation is host-tested (43 debug cases, 89 focused, 213 full frontend tests, typecheck/export at its checkpoint); its first independent review hit provider 429 before producing a verdict. The same-route retry has now returned Spec PASS / Quality APPROVED in task-3b-main-independent-review.md. Parent freshly verified all five source hashes against its review manifest; tests were not rerun during this read-only review/planning pass. No live child remains at the latest list check. Task 3C actual callback queue/GATT closure and remaining Task 4B boundary/history checks still gate whole Task 3/4.

Checked source anchors:
- components/device/WifiSettingsDialog.tsx:32 creates its own BLE client; :73-82 clears the password only after successful await. Route app/devices/[id].tsx:53 creates another client. Task 5 must replace both ownership sites, not merely add a third wrapper.
- CalibrationWizard.tsx:23-34 defines the actual BleCalibrationAdapter; :170-184 owns busy/session state and :200-217 persists command/session IDs only. Preserve that boundary; never copy operatorToken/csrfToken into debug authorization.
- SigapApp::loop at firmware/src/app/sigap_app.cpp:623 has BLE/maintenance at :636-637, acquisition at :648, storage exclusion at :656-668, calibration expiry at :689, and media migration at :697. Existing archive/control busy checks do not cover the proposed debug task. Audit each actual I/O caller rather than declaring every poll an I/O operation.
- ApplicationCalibrationExecutor::execute is in firmware/src/app/device_control_executor.cpp:301, with controller_.execute at :393. Include the cpp in the admission audit; the declaration alone cannot prove state changes are prevented.
- Existing transport headers are firmware/lib/network/src/network/{https_transport.h,device_control_http.h}; device_control_http.h is not in firmware/src/app/. Neither exposes the proposed debug quiescent() contract. New transport teardown semantics need actual implementation/tests.
- ProcessingRuntime::observe forwards to downstream_observer_ before its own processing, at processing_runtime.h:121-124. The app binds evidence_cache_ at sigap_app.cpp:283. Task 7 must preserve that exact chain.
- Existing SHA-256 helper is sigap::telemetry_contract::sha256(std::string_view), declared in firmware/lib/telemetry_contract/src/telemetry/sha256.h. Reuse it read-only; do not alter telemetry contracts or substitute archive ACKs for debug ACKs.
- Backend HistoryPage is the flat JSON shape in internal/devicedebug/contract.go:55-75, including snapshot_level, captured_monotonic_us, oldest_sequence, overwritten, delivery_lost, next_after, oldest_available, retention_gap and has_more. Task 8 must not invent nested event DTOs or treat ingestion_id as source sequence.

## Readiness and dependency graph

Prepared does not mean released or implemented.

    Task 3C + whole Task 3 review ----> Task 5A -> 5B -> 5C -> whole Task 5 review
                  |                                      |
    whole Task 4 review ------------> Task 6A -> 6B -> 6C1 -> 6C2 -> 6D -> whole Task 6 review
                  |                                                        |
                  +--> Task 8A, only when npm ownership is free              +--> 7A -> 7B -> whole Task 7 review
                                      |                                           |
                                      +------------- 5 accepted + 7 accepted ----> 8B -> whole Task 8 review
                                                                                         |
                                           all 1-8 accepted ----------------------------> 9A -> 9B -> review -> 10A

Task 8A is only the authenticated history API/DTO consumer. It may be released after Task 4's contract is accepted and the npm owner is free, while firmware work continues. Task 8B and whole Task 8 keep the original Task 5/7 integration gates. Cost if the backend changes after this early stable-contract slice: redo its strict DTO tests; no speculative UI or firmware binding is approved.

Safe execution waves:
1. Release Task 5A (TS/npm) as soon as Task 3 is accepted; do not unnecessarily wait for independent Go Task 4B. Release Task 6A (C++/.pio) once its Task 3/4 gates both pass, and pair 5/6 when both are ready.
2. Continue 5B/5C on the same exclusive npm seat while 6B/6C1/6C2/6D use the exclusive firmware seat. Only one active owner per shared source family and build output.
3. With Task 5 released and npm free, run 8A alongside 7A/7B firmware work. No competing app/firmware composition owners.
4. Finish 8B after Task 7 acceptance, then run cross-language integration and the serial target-build/browser stages of Task 9.
5. Whole-feature review/handoff follows actual source-bound integration evidence; never replace a missing per-task review with a test count.

## Review focus and checkpoints

The original plan owns exact interfaces and executable TDD examples; this addendum narrows scheduling and caller coverage rather than defining alternate APIs.

- RF1, new command ordinals and real callback dispatch: Task 3C/whole Task 3 must prove debug never enters calibration or renews its lease before Task 5 uses the client.
- RF2, completed/cancelled HTTP with teardown still pending: 6A/6B/6D keep VFS ownership until actual quiescence, without pausing acquisition/controller expiry.
- RF3, old promise or lease completing after a new context: 5A/5B and 8B pin password/error/activity ownership and forbid stale fetch/timer/append.
- RF4, full/filtered ring while mandatory samples arrive: 7A and 6D preserve the exact evidence destination, handoff ownership and acquisition sequence.
- RF5, multi-boot/device history beyond Number precision: 8A/8B/9A preserve source uint64 versus server cursor/receipt semantics and real retention notices.

- [ ] Before 5/6: relevant Task 3/4 gates, stable interfaces, path collisions, exact model route and resource ownership rechecked.
- [ ] After 5: actual shared-owner/controller/render checks and independent spec/quality review accepted before 8B.
- [ ] After 6/7: actual teardown/admission/composition/link evidence and independent reviews accepted; no physical claim.
- [ ] After 8/9: cross-language integration, serial target builds and permitted browser artifacts accepted; oversized-document gate explicitly resolved.
- [ ] Before 10 handoff: source-bound whole review complete; unresolved physical effects stay HOLD and get a separate narrow permission request.

## Task 5: one production BLE owner and Wi-Fi lifecycle

### 5A: shared DeviceBleSession
Files: new lib/ble/device-session.ts and device-session.test.ts. Read-only inputs: current lib/ble/client.ts and CalibrationWizard's adapter type. Output is the proposed DeviceBleSession contract in the original Task 5 brief, not an existing symbol.
- Implement one PublicBleClient factory instance per page context, immutable snapshots/subscriptions, connect/run/close/dispose and independent BLE/HTTPS control-activity leases.
- Tests: one construction across Wi-Fi/calibration/debug; absent expected BLE identity rejects before chooser; overlap rejection; deliberate reconnect versus unexpected disconnect; stale/idempotent release never clears a newer or HTTPS lease; close preserves HTTPS activity, dispose invalidates all.
- Verify npm exec -- vitest run lib/ble/device-session.test.ts, then typecheck. Full frontend checks run after final slice edits. Exit: tested session API and complete typed fake client, no UI edits.

Production options are fixed, using the already implemented client interface:

```ts
const options = {
  readOnlyMaxAttempts: 3,
  commandTimeoutMs: {
    GET_DEVICE_ID: 30_000, GET_STATUS: 30_000,
    SCAN_WIFI: 30_000, GET_DEBUG: 30_000,
  },
};
```

Do not set global requestTimeoutMs=30000; calibration and mutations keep their existing deadline/one-send semantics.

### 5B: WifiSettingsController
Files: new lib/ble/wifi-settings.ts and wifi-settings.test.ts, consuming the reviewed 5A API.
- Own open/close/password/selection/status/error and pending generations. Clear password before starting submit, on disconnect and close, never from an old promise's unscoped finally.
- Tests: late resolve AND reject after new session/password; error survives busy cleanup; valid hidden scan rows filtered only after full count/structure validation; reconnect rebases only the intentional attempt.
- Verify npm exec -- vitest run lib/ble/device-session.test.ts lib/ble/wifi-settings.test.ts. Exit: real controller tests use DeviceBleSession, not an unrelated fake public controller.

### 5C: production component adapters
Files: existing WifiSettingsDialog.tsx, CalibrationWizard.tsx, app/devices/[id].tsx; tests/routes/detail.test.ts and new tests/device/session-view.test.tsx.
- Replace both inspected client constructors with page owner and adapters; keep explicit shared disconnect control and route/device disposal. Wizard acquires one activity lease over its whole busy/active-session interval, not each state transition. Dialog close clears view secrets without orphaning or closing unrelated page activity.
- Tests exercise actual component props/SSR plus controller lifecycle, not only source strings. Preserve existing command/session sessionStorage; no passwords/tokens there.
- Verify focused 5A/5B/render/route tests, full npm suite, typecheck and export separately. No DOM package installation. Exit: whole Task 5 spec/quality review and frozen interface for Task 8.

## Task 6: debug HTTPS with real teardown/I/O ownership

All names below are proposed Task 6 outputs unless identified as existing. C++ changes and shared .pio are serialized. Do not dispatch the entire admission/app surface as one uncontrolled broad worker.

### 6A: pure worker and RuntimeIoGate
Files: new debug/io_gate.h, http_worker.h/.cpp; new native/test_debug_io_gate/test_debug_io_gate.cpp and native/test_debug_http/test_debug_http.cpp.
- Freeze RuntimeIoGate's IoOwner/try_acquire/release/active contract and worker/readiness/result types from original Task 6. Debug excludes all other owners; Filesystem excludes HTTP; preserve existing archive/control coexistence policy.
- Freeze one service snapshot, at most 8 events and 4096 exact bytes. Pack a fitting prefix before hashing with the existing SHA helper. Validate debug ACK by matches_upload_ack against the actual frozen identity/snapshot/digest.
- Tests: exact bytes over retries, ring overwrite in flight, matching/wrong/truncated/202 ACK, offline/time/CA/credential readiness, stop on 401/schema/conflict across ordinary reconnect, exactly 3 attempts, >=5-second cadence/deadline and capped 5-to-60-second backoff. Exhausted final delivery advances this debug cursor/loss only, not USB/BLE/archive.
- Distinct fake cancellation and quiescence states prove cancel is not completion; gate retention must be tested through worker/app ownership, not by manually releasing a fake gate.
- Verify full-path native/test_debug_io_gate and native/test_debug_http separately. Exit: stable pure interfaces and meaningful RED/GREEN coverage.

### 6B: owned asynchronous transport
Files: new debug/http_transport.h/.cpp; focused Arduino/FreeRTOS/HTTP shims and the original planned debug_app probe/test where needed. Read existing network/{https_transport,device_control_http}.h/.cpp, without copying their ACK type.
- One owned asynchronous task; copied bounded endpoint/token/body lifetime until teardown; no synchronous request from poll, BLE callback or acquisition. CA/time gating is retained. Missing configuration stays unavailable.
- Tests separately hold response completion, cancel request and TLS/VFS cleanup, verify quiescent is published only after cleanup, byte/token lifetime and zeroization. No real credentials or live TLS claims.
- Verify the actual transport translation unit through SDK-shaped host shims and the focused worker suite. Exit: concrete start/poll/cancel/quiescent implementation matching 6A.

### 6C1: maintenance mutation admission
Files: existing maintenance/runtime.h/.cpp and runtime_calibration.cpp; model_runtime.h/.cpp only for traced I/O reachability; extend the owned debug admission tests without expanding the accepted maintenance suite.
- Bind one application-owned gate to real USB/router/profile/model/config/storage-probe entry paths. Reject/defer while DebugHttp owns the slot BEFORE changing workflow/controller/media state. Trace model polling: gate only actual I/O, never pause RAM-only expiry.
- Tests count real config/secret/profile/model/probe/audit operations and calibration owner state before/after denial; no partial draft or lease acquisition, no implicit retries, normal admitted path still works.
- This intentionally extends accepted Task 2 consumer files for I/O admission; record exact before/delta and preserve debug command/USB codec behavior. Do not claim all old 17 whole-file hashes remain equal afterward.
- Exit: named admission binding API and tested maintenance consumers; one owner serializes these files.

### 6C2: BLE and HTTPS calibration admission
Files: maintenance/ble_calibration_backend.h/.cpp and app/device_control_executor.h/.cpp, plus actual admission probe/tests.
- Use the same gate and semantics as 6C1 before execute_controller/controller_.execute and setup-field application. Planning inspected the real executor implementation at :301/:393; do not implement a declaration-only guard.
- Tests: debug pending/cancelled-not-quiescent denies mutation with no controller/lease/media deltas; ordinary calibrated workflow and replay/expiry remain intact after release. Synthetic host commands only.
- Exit: both transport entry boundaries use the same admission contract; no new authorization policy or duplicate calibration controller.

### 6D: full application composition
Files: app/sigap_app.h/.cpp, archive_runtime.h as needed, planned debug_app actual-production probe/shims and tests/test_debug_app_composition.py.
- Bind gate/worker/transport and 6C consumers to one owner. Audit loop's maintenance calls, storage/batch work, media migration and profile paths. Archive/control get priority before starting debug; their new starts defer while DebugHttp is active.
- Tests keep a real application poll path active across cancellation and completion cleanup: zero forbidden storage/profile/probe calls, continued acquisition/controller.poll, unchanged acquisition sequence/mandatory record ownership, release only on quiescence.
- Verify Python actual-app composition unittest and the focused native worker/gate/maintenance/BLE suites serially. Exit: whole Task 6 independent review, not merely standalone gate tests.

## Task 7: real firmware instrumentation and explicit full-app debug build

### 7A: observer and typed module outcomes
Files: new app/runtime_debug_observer.h/.cpp and native/test_runtime_debug_observer/test_runtime_debug_observer.cpp; actual module emit sites remain under this firmware owner's exclusive scope.
- Implement existing acquisition::ISampleObserver using debug::IEventSink; forward the exact sample once to the original evidence observer even when logging is filtered/full. No acquisition sequence allocation or handoff pop.
- Pin all seven module areas to the frozen registry: runtime, BLE, Wi-Fi, storage, acquisition/processing, calibration/profile/model, HTTPS. Emit only actual transitions/typed numeric reasons, not raw values/SSID/token/free-form errors. GET_DEBUG/level/upload observation does not recursively log itself.
- Tests: exact once forwarding/address identity, full ring, repeated real transitions versus one-shot latches, privacy/registry/level bounds. Verify native/test_runtime_debug_observer and actual-app probe.

### 7B: application bindings and target build
Files: app composition, actual outcome sites, build/build_config.h, platformio.ini and docs/TASK9_Customization.md. Do not overlap any Task 6 composition owner.
- Preserve acquisition -> ProcessingRuntime -> RuntimeDebugObserver -> original evidence chain. Bind all required production sinks; isolated diagnostics explicitly omit unsupported components.
- Add sigap_n16r8_debug inheriting the actual full application, INFO production/DEBUG explicit build, volatile local runtime level. Verify diagnostic sources and erase wrappers do not enter full-app link; preserve effective MAX_BONDS=0/PERSIST=0.
- Serial no-upload build and native/composition checks; report BIN/ELF/map, buffers/stack/static size deltas. Target heap/timing remain NOT_RUN without separately allowed HIL.
- Exit: whole Task 7 review and source/link evidence; never an installed image claim.

## Task 8: authenticated history and real website debug view

### 8A: strict authenticated API consumer
Files: new lib/api/debug.ts and debug.test.ts. Existing OperatorAuthorization/Fetcher/ApiError definitions are read-only inputs. No token sharing with the wizard.
- Implement the original createDebugApi/history signature and flat HistoryPage shape from actual Go contract.go:55-75. GET only, missing authorization rejects before fetch, URL device identity is exact, limit<=100 and canonical signed-int64 server cursors; source uint64s remain strings.
- Tests consume actual Go history output shape, including snapshot_level versus event level, received_at versus monotonic times, optional null/zero, unknown/malformed fields, >2^53/max values and wrong device. No Number conversion or alternative nested DTO.
- Verify npm exec -- vitest run lib/api/debug.test.ts and typecheck. Exit: API slice can be prepared while firmware proceeds only after Task 4 contract acceptance and exclusive npm availability.

### 8B: cancellable controller and panel integration
Files: new lib/debug/poller.ts/poller.test.ts, DeviceDebugPanel.tsx, tests/device/debug-panel.test.tsx; route modification follows Task 5 ownership handoff and Task 7 release.
- One visible/connected/identity-matched/idle refresh with >=5-second interval; sequential BLE META/PAGE under session.run('debug'). Abort HTTPS and invalidate timers/append on hidden/source/device/disposal generation changes. Control-activity leases pause both sources.
- Cache<=200 events keyed by exact device+boot+sequence, not ingestion ID. Refresh META on boot change; server ID holes alone are not loss. Explicit BLE/HTTPS selection, no fallback/chooser, unsupported reasons, read-only remote level, real local ACK level.
- Tests defer source/device changes, calibration leases, hidden page/abort/unmount, precision/dedup/gaps, unavailable firmware/session, and actual SSR props. Keep Firmware debug separate from Client trace (<=60); token explicit in-memory and cleared on context disposal.
- Verify focused API/poller/panel + full npm/typecheck/export separately. Exit: whole Task 8 review; browser/radio acceptance is not established by SSR.

## Task 9: integration and staged artifacts

### 9A: actual producer -> real handler/store -> actual website controllers
Files: original planned internal/devicedebug/integration_test.go, tests/test_debug_end_to_end.py, tests/debug/production-flow.test.ts and actual probe readers.
- Recompile actual C++ producer; verify frozen fixture bytes and storage-only final-LF rule; use exact transmitted request bytes/digest through isolated real auth/HTTP/SQLite and then strict TS history/controller paths.
- Tests: changed hash/identity/sequence fails without partial rows, unmodified output commits, first receipt/dedup stable, multi-boot/device precision/retention, old generation rejection. All injected evidence labeled TEST.
- Verify focused checks then full Go unit/race/vet, full frontend checks, audited Python unittest discovery and full native suite. Real commands/counts/skips/failures, not old totals.

### 9B: serial linked targets, permitted browser smoke and artifact manifest
- Build each original seven target environments separately, without upload: sigap_n16r8, sigap_n16r8_sd, sigap_n16r8_https_compile, sigap_n16r8_sd_https_compile, sigap_n16r8_debug, sigap_ble_diag, sigap_ble_wifi_test. Debug target becomes executable only after Task 7 creates it.
- Browser smoke uses an isolated explicitly permitted local instance/temporary DB at desktop1920x1080 and mobile390x844. Confirm permission before starting listeners or touching an existing server. No automated physical chooser/login or production tokens; SDK/vault workflow if operator interaction is required.
- Stage actual BIN/ELF/map/web/source hashes/exit logs under the approved artifact root, preserve the diagnostic/helper baseline. Distinguish exported/served/open-tab states; no automatic activation.
- Startup audit covers constructors/framework/config/NimBLE/Wi-Fi, pins, NVS and media before any physical permission request.
- Documentation gate: README/Customization/web-calibration are below1500 at this snapshot; task9-verification is1649. Keep evidence artifacts without editing that oversized document; obtain narrow permission before its required expansion/split. Do not mark all Task 9 criteria complete while that required update is unresolved.

## Task 10: independent acceptance and permission-scoped handoff

### 10A: whole-feature review and exact final gate report
- A fresh independent reviewer consumes approved spec, actual scoped/full deltas including untracked files, actual test/link artifacts, and all per-task reviews/fixes. Verify RF1-RF5, typed privacy, quiescence/admission, lease/sequence/archive safety and no-passkey build evidence.
- Findings get precise regression/correction and scoped re-review; cannot waive an open load-bearing defect or invent a passed review during provider429. Preserve source/tests and stop that review, without silent provider substitution.
- Final report separates implemented, host-tested, linked, deployed, physically verified and USER_REPORTED. HEAD/dirty state/no Git writes/no flash remain explicit; artifact paths/hashes and outstanding docs/permissions are named.
- Prepare only a narrow physical request for the chosen image/startup effects/flash-readback/observation/stop rules. No serial monitor, sensor/actuator/calibration/NVS/SD mutation or physical execution until separately authorized. HIL stays HOLD.

## Verification commands carried into every dispatch

Use explicit root workdir and one actual project command per terminal call. These are planned commands, NOT results of this planning pass:

```text
npm exec -- vitest run lib/ble/device-session.test.ts lib/ble/wifi-settings.test.ts
npm test -- --run
npm run typecheck
npm run export:web
/home/raya/.platformio/penv/bin/pio test -d firmware -e native -f native/test_debug_http
/home/raya/.platformio/penv/bin/pio test -d firmware -e native -f native/test_debug_io_gate
/home/raya/.platformio/penv/bin/python -m unittest discover -s tests -p test_debug_app_composition.py -v
env GOTOOLCHAIN=local /home/raya/.cache/sigap-toolchains/go1.27.1/bin/go test -mod=readonly ./... -count=1
env GOTOOLCHAIN=local /home/raya/.cache/sigap-toolchains/go1.27.1/bin/go test -race -mod=readonly ./... -count=1
env GOTOOLCHAIN=local /home/raya/.cache/sigap-toolchains/go1.27.1/bin/go vet -mod=readonly ./...
/home/raya/.cache/sigap-toolchains/go1.27.1/bin/gofmt -d internal/devicedebug/integration_test.go
```

Each ready brief supplies its exact focused test paths. New suites are not expected to exist until their owning implementation creates them. Zero discovered tests never establishes PASS.

## Dispatch preparation and supervision workflow

1. While an agent implements or reviews, main prepares the next task's source map, caller/consumer contracts, test cases, resource ownership and report path only. Do not edit the active agent's files, snapshot half-written source, run its builds or access hardware.
2. Ready headers are lookahead/task-5-ready.md through task-10-ready.md; each points to the exact extracted original task-N-brief.md. Read one task's header/excerpt/current constraints, not the entire accumulated session or every old recovery report.
3. Before dispatch: recheck Git/HEAD, current relevant source hashes/line budgets, prerequisites, proposed-path collisions, route and owner permissions. Review changes since this planning snapshot; a READY package is not an implementation authorization. Release groups with more than about five checked files must be narrowed by responsibility before dispatch rather than resending an oversized catch-all; the same worker still owns implementation plus its covering tests.
4. Freeze each producer contract before its consumer starts. Record explicit downstream consumer extension deltas; never enforce obsolete whole-file hashes to prohibit approved wiring, nor silently thaw schemas/core producers.
5. Save before copies using .snapshot suffix and separate original source paths in manifests. Recheck test discovery; old *.test.ts copies in .superpowers inflated counts previously.
6. Edit existing code with unique-literal-context V4A patches after reading. After two same-region validation failures, reread and use the allowed enclosing-file rewrite recovery; no repeated generic-brace hunks or source edits through shell scripts. Re-run the final applicable suite after the last edit; preserve honest coverage-GREEN versus actual behavioral RED/GREEN.
7. While reviews run, continue preparing the next stable wave. If quota blocks review, retain its pending status and continue authorized planning; do not replace independent review with main self-review or silently change providers.
8. Applicable execution skills: using-superpowers, sigap-task9, user-engineering-standards, installed ponytail full (no upstream fetch), test-driven-development, verification-before-completion and subagent-driven-development. Add frontend-ui-engineering for 5/8, observability-and-instrumentation plus systematic-debugging for 6/7, and code-review-and-quality/requesting-code-review for independent gates. Load only the selected slice's needed source references; never install/update skills or follow a denied native/fetch action.

## Prepared output and remaining gates

This addendum and six original extracted briefs plus six ready headers are PLANNING artifacts. Source inventory/dependency index live under the existing SDD workspace's lookahead directory. No Task 5-10 application code, new runtime module, server, build target, test result or physical feature is claimed implemented here. Whole Task 3/4 completion/review still controls release. The returned Task 3B slice review was independently read and hash-checked, but it does not approve queued firmware Task 3C or whole Task 3.
