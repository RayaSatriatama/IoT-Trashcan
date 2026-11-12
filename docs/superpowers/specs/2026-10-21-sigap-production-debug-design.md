# SIGAP Production BLE Integration and Firmware Debug Design

Date: 2026-10-21, WIB.
Status: written specification approved by the owner on 2026-10-21; written implementation plan and source/host execution approved through `lanjutkan` on 2026-10-22 WIB. Host implementation is starting; physical deployment remains separately gated.
Classification: architectural, because firmware, BLE, HTTPS, backend persistence, and website interfaces change together.
Repository: `/mnt/nvme0n1p4/Projects/Programming_Projects/full-integration-hil-validation`.
Inspected HEAD: `0257b9e5cc08b142348f85f6da184842c09ab420`, branch `feat/full-integration-hil-validation`. Existing modified and untracked work must be preserved.

Normative/source references:

- `docs/Microcontroller/PRD_Firmware_SIGAP_ESP32S3_PlatformIO.md`
- `docs/Microcontroller/TSD_SIGAP_ESP32S3_PlatformIO.md`
- `docs/TASK9_Customization.md`
- `docs/task9-ble-manual-testing.md`
- `docs/task9-ble-no-passkey-verification.md`
- `contracts/telemetry/v3/schema`

Current explicit owner limits govern prototype BLE security and physical authorization. Preserve other normative contracts and verified source behavior; do not treat historical passkey instructions as permission to reverse the approved prototype policy.

## 1. Intent and accepted scope

Integrate the operator-accepted BLE/Wi-Fi helper behavior into the real SIGAP website and full application firmware. Add firmware-originated debugging that can explain an operation from start to completion or failure, not merely announce boot readiness.

The owner approved USB + local BLE + remote HTTPS access. BLE provides local access when the device's Wi-Fi is unavailable; HTTPS provides remote access when device connectivity, time, CA, credentials, and backend are ready. Browser-side request traces remain useful but are not substitutes for firmware events.

Success means a matching operation can be followed through the application UI, actual client codec, firmware dispatcher, underlying module, and terminal result. Host tests, linked builds, local serving, deployment, and physical acceptance remain separate evidence categories.

### Non-goals and permission boundaries

- No BLE passkey, setup-secret gate, or new BLE application authentication for this prototype. A matching self-reported identity remains a routing check, not authentication.
- Do not remove HTTP authorization, device credential checks, operator permissions, CSRF protection on existing mutations, or TLS verification.
- No automatic flash, reboot, partition modification, erase, provisioning save, profile/calibration COMMIT, SD write, heater activation, sensor/actuator physical test, firewall change, or public deployment.
- Do not copy diagnostic-image NVS erase guards or DHCP-cache wrappers into the full application. Preserve normal authorized AppConfig/profile persistence separately from BLE security storage.
- Do not change telemetry-v3 schemas, immutable pending bodies, ACK/reclaim rules, research evidence, FIS/safety ownership, or acquisition scheduling to accommodate debug logs.
- No durable debug log on the MCU, core-dump partition, raw UART forwarding, browser-to-server firmware-log relay, or new remote debug-level mutation endpoint in this increment.
- Reuse the pinned toolchain/dependencies. Do not upgrade the SDK, replace the web framework, or add a logging platform to deliver this feature.
- No commit, staging, push, merge, reset, stash, or cleanup of unrelated work.

## 2. Current source and reason for the change

The inspected entry points are `components/device/WifiSettingsDialog.tsx`, `app/devices/[id].tsx`, `lib/ble/client.ts`, `lib/ble/protocol.ts`, `firmware/src/app/sigap_app.cpp`, and the provisioning modules.

The shared client already contains helper-era framing, hidden-SSID, tracing, and retry repairs. That does not prove the production caller supplies the accepted policy or owns its asynchronous lifecycle correctly. The Wi-Fi dialog and calibration page currently construct separate BLE clients; these must not compete for the same page's physical session.

Production firmware has one-shot `RuntimeDiagnosticLatch`/runtime markers. These do not provide complete repeated-operation and transition visibility. Preserve existing commissioning markers while adding structured events; do not silently invalidate their observers.

Source-proven integration risks also include BLE logical idle expiry without corresponding GATT closure and scan status collapsing distinct outcomes into generic failure. These need focused source-level regressions, not assumptions about a new physical crash.

The full application's HTTPS workers share the VFS descriptor domain with storage. `SigapApp::loop()` already gates filesystem work while transports are active. A debug worker must participate in that exclusion, including cancellation/cleanup, rather than reopening the descriptor race.

## 3. Architecture and alternatives

Selected data flow:

    module transition/outcome -> typed DebugService -> bounded RAM ring
                                                   -> bounded USB sink
                                                   -> read-only BLE cursor page -> website
                                                   -> async HTTPS upload -> Go/SQLite -> website

The firmware DebugService owns event sequence allocation, level filtering, and ring state. Each sink has an independent cursor. Reading USB or BLE never consumes the HTTPS cursor or removes an event. HTTPS captures a bounded immutable in-flight snapshot before releasing the ring lock.

One page-scoped BLE session owner supplies the Wi-Fi dialog, calibration UI, and debug panel. The existing MCU calibration controller remains authoritative across USB/BLE/HTTPS; the browser does not become a second calibration authority.

Alternatives considered:

- USB + BLE only: smaller scope and independent of Wi-Fi, but insufficient for approved remote access.
- USB + HTTPS only: supports remote access, but loses the local firmware diagnostic path during provisioning/connectivity failure.
- Selected USB + BLE + HTTPS: one event model across sinks, with additional bounded transport and persistence work. Avoid separate log formats, mandatory cloud availability, or an unrestricted logging platform.

## 4. Firmware event and storage contract

Introduce a versioned debug-v1 contract separate from telemetry-v3. New package/file names in this document are proposed implementation areas, not claims that those APIs already exist.

### Identity and typed fields

- Reuse the existing `acquisition::BootRuntimeIdentity::boot_id`. Debug has its own uint64 sequence; it must not consume the acquisition/archive sequence allocator.
- Wire `boot_id` uses the existing canonical representation of the 16-byte BootId. Do not invent a second boot identity.
- HTTPS `device_id` is the exact backend `device_code` authorized for the existing device credential. Do not derive or normalize it from the advertised name, MAC display, database row ID, or BLE wire ID. Website routing uses the existing explicit identity mapping.
- An event has uint64 `sequence` and `monotonic_us`, registry enums `level`/`module`/`event_code`, optional numeric BLE `request_id` using the existing command-ID range, uint32 `duration_us`, two int32 arguments, and optional uint64 `related_sequence` referring to actual acquisition evidence. Presence bits distinguish absent values from zero. Opaque HTTPS/control command IDs are not truncated or converted into invented numeric request IDs; correlate those operations through their existing command/status evidence.
- Module/event codes and argument meanings are allowlisted in one versioned registry with cross-language fixtures. They are not arbitrary messages or metadata dictionaries. MCU event records contain no string allocations or payload copies.
- All uint64 wire fields are canonical decimal strings, without leading zero except `0`; consumers must not round them through JavaScript Number. Boot ordering is explicit; MCU monotonic time is not UTC. Backend `received_at` means receipt, not sensor acquisition time.
- Build identity and effective debug level appear in bounded status/envelope metadata. Human-readable text is rendered from the registry, not captured from free-form exceptions.

### Privacy

Never include frames, response bodies, SSIDs, passwords, tokens, setup secrets, NVS values, bond data, HTTP headers, secret-bearing URLs, or free-form error text. Normal intentional Wi-Fi UI data is not a debug log and is not removed by this rule.

Code insertion points pass only allowlisted numeric/status metadata. Do not enable global SDK verbose logging as the implementation of debug mode. The safe USB capture accepts only the structured prefix and explicitly allowlisted boot metadata; it does not persist arbitrary vendor UART output.

### Proposed initial hard limits

These are design defaults, not measured HIL performance claims:

| Boundary | Limit/default |
| --- | --- |
| RAM event ring | 64 records; maximum 64 bytes per stored record, checked at compile time |
| Ring record storage | At most 4096 bytes, excluding service state and sink buffers |
| BLE page | At most one event; complete response within the existing 512-byte response boundary |
| HTTPS snapshot | At most 8 records; at most 4096 serialized request bytes; pack fewer if needed |
| HTTPS outstanding request | One debug request, owned by one asynchronous worker |
| HTTPS upload cadence | No more frequently than every 5 seconds |
| HTTPS request deadline | 5 seconds per upload attempt |
| HTTPS attempts | At most 3 total for the same immutable in-flight snapshot; backoff starts at 5 seconds, doubles, and is capped at 60 seconds |
| Browser client trace | Existing 60-event in-memory limit |
| Browser firmware view | At most 200 displayed events; explicit older-event removal |
| Backend query page | At most 100 events |
| Backend retention | 24 hours of received debug history, at most 2000 events/device and 20000 events globally |

Budget ring storage, in-flight copies, codecs, worker stack, and TLS allocations separately. A linked map/size report must show the incremental cost. TLS heap/stack headroom and timing remain unverified until authorized HIL; ring size alone is not total memory usage.

Ring overflow drops oldest debug records and exposes overwrite counts, oldest available sequence, and cursor gaps. It never overwrites mandatory telemetry pending records or changes `LOGGING_BLOCKED` semantics. A cursor from a different boot produces an explicit boot-change result, not invented continuity. Sequence exhaustion must be explicit and must not wrap into reused IDs.

Producers use a short, bounded synchronization boundary to append/copy fixed-size records. No USB, filesystem, JSON, network, allocation, or waiting for sink readiness while holding it. IRQ-context production is excluded unless a separately tested IRQ-safe adapter is provided.

### Levels and feedback-loop prevention

The production default is INFO; an explicit full-application debug build defaults to DEBUG. Runtime USB/BLE level selection is volatile and bounded to ERROR, WARN, INFO, DEBUG. It does not touch AppConfig, profiles, NVS, sensor schedules, calibration ownership, or leases. Restart returns to the build default. HTTPS is read/upload-only for this increment; remote operators receive the level selected by the build or local operator.

Keep ERROR/fault visibility in the minimum level. INFO covers boot phases, state transitions, and terminal outcomes. DEBUG adds request/framing counts, durations, and sampled health/counter summaries. Repeated identical conditions are coalesced with counters. Reading debug pages and uploading debug records must not generate routine per-request events that recursively refill the same ring; failures/transitions are rate-limited, and sink counters are readable out of band.

## 5. Operational coverage

Each module reports real state, bounded counters, timing, and typed failure reason at its actual boundary:

| Area | Required observations |
| --- | --- |
| Boot/runtime | Build and boot identity, supported SDK reset reason, setup stages, loop/worker health, readiness/failure transitions |
| BLE | Connect/disconnect, ordinary command start after complete parse/reassembly (not partial F1 writes or debug ACKs), pending/completion/rejection, request ID, bytes, and outbound response chunks admitted by successful SDK notify calls (not radio/client delivery), duration, idle expiry. Unknown link reason is 3; pending count is session-owned admitted async logical backend requests awaiting completion (currently scan 0/1, snapshotted before cleanup), not fragments, rendered replies, or an unobserved SDK queue. |
| Wi-Fi | Scan start/pending/ready/rate-limit/failure, association/reconnect transitions, result counts and durations; no network identity/credential |
| Configuration/storage | Init/mount/recovery/result, queue pressure, append/ACK/reclaim outcomes and faults; no persisted payload |
| Acquisition/processing | Driver quality/reason transitions, missing/stale/uncalibrated channels, counters/backpressure and true processing availability; no extra physical reads |
| Calibration/profiles/models | Ownership/busy/expiry/cancel/result and validation/version state; no automatic capture, fitting, save, COMMIT, or replacement of existing evidence |
| HTTPS | Time/CA/credential readiness, request/ACK/retry/failure class, latency and queue state; no body/header/token |

Correlate a request across applicable boundaries; do not fabricate request IDs for unrelated background work. Scientific/raw sample inspection continues through the actual acquisition/research evidence paths. Debug summaries do not manufacture conversions, coefficients, classifications, raw samples, or successful acquisition when unavailable.

The RAM ring is lost on reset/power loss. Web can show new-boot/reset metadata and server-received prior events, not a guaranteed pre-crash tail, ROM console history, or core dump. An observed USB disconnect is not itself a proven MCU crash.

## 6. USB and BLE transport

USB uses a distinct `SIGAP_DEBUG` structured-line prefix with bounded output. Keep existing runtime/commissioning marker compatibility. USB absence, a full host buffer, or a slow reader cannot block the main loop; sink lag/drop state is visible. No infinite flush/wait loop.

Add typed `GET_DEBUG` and `SET_DEBUG_LEVEL` operations to the existing command codec/dispatch path. Both reuse existing framing; do not enlarge the 512-byte response or invent a second GATT service. `GET_DEBUG` has two explicit modes: META returns boot/build identity, effective level, sequence bounds, and sink/loss counters without an event; PAGE supplies expected boot and after-sequence and returns boot identity, effective level, oldest/next sequence, overwrite/gap state, and zero or one event. Build identity is limited to 96 ASCII bytes and belongs to META, not a conditionally present page field. The website fetches META at connection and again on boot change. Oversize is an explicit encoder failure, never truncated valid-looking data.

The exact command/response field encoding and registry are captured in committed actual-encoder/actual-parser fixtures before consumers are switched. Include worst-case uint64/argument values, empty page, stale cursor, boot change, unknown/malformed fields, and boundary-size packets. Terminal-LF removal, request-ID isolation, progress ACK filtering, and maximum F1/R1 frame counts remain intact.

`SET_DEBUG_LEVEL` executes once and acknowledges the actual effective level; UI state is not optimistically relabeled as a firmware acknowledgement. It is not a profile or calibration mutation and must not renew their ownership leases. New `GET_DEBUG` reads use a 30-second deadline with one attempt; the existing owner-approved three-attempt allowlist is not silently widened.

Idle BLE expiry closes the actual GATT connection and clears pending transport state exactly once. Do not leave a logically disconnected session behind an open GATT connection or depend on a terminal error whose fragments can no longer drain. Preserve the existing timeout policy rather than disabling it to hide the defect. Distinguish scan rate limiting/device rejection from actual request timeout.

## 7. HTTPS and backend

### New additive routes

- `POST /api/device-debug/device/{device_id}/events`: device-authorized firmware upload.
- `GET /api/device-debug/events/{device_id}`: operator-authorized, per-device paginated history.

Compose through `internal/http/router.go` and `cmd/sigap/main.go`, with a proposed `internal/devicedebug` service/store/handler boundary. Reuse the existing `devicecontrol.DatabaseAuthorizer` instance and its operator/device interfaces; nil, missing, expired, revoked, wrong-device, or invalid authorization fails closed. Do not create a permissive fallback or reset shared rate quotas through a second independent authorizer.

Device auth remains bound to `devices.device_code`. Require exact path/body identity equality. No token creation/rotation or reading live credential values is needed to implement/test these interfaces. Tests use isolated disposable credentials and databases.

### Upload semantics

The debug-v1 envelope contains schema version, exact device identity, boot identity, build identity, effective level, `captured_monotonic_us` measured when the snapshot is captured, snapshot/cursor and loss metadata, and a bounded event array. Event identity is `(device_id, boot_id, sequence)`. Snapshot uptime allows old events to be distinguished from newly captured ones without inventing MCU UTC.

Validate UTF-8, depth, body/record/array limits, duplicate JSON keys, trailing input, canonical integers, enums, argument types, and unknown fields. Reject the entire batch on malformed data. Within retained history, an identical validated event is deduplicated; a different event for the same identity returns conflict with no partial insertion. Durable SQLite transaction commit precedes success acknowledgement. The firmware advances only through the matching accepted snapshot; a lost ACK retries the same captured events/bytes, not a reconstructed changing body. All envelope metadata is frozen with that snapshot too. Debug retention does not provide permanent replay/conflict detection after the relevant history is pruned; do not introduce an unbounded tombstone ledger or claim telemetry-style indefinite idempotence.

After the upload attempt budget is exhausted, stop retrying that snapshot, increment delivery-loss counters, and advance the debug upload cursor explicitly. Preserve any surviving RAM events for USB/BLE. Overflow and delivery-loss remain visible even though debug delivery is best-effort. No claim of complete history or telemetry-style durable delivery.

Use bounded 4xx/5xx failure classes. Stop automatic attempts on authorization, schema, or conflict rejection until the relevant validated configuration/credential generation changes or an explicit operator restart; ordinary Wi-Fi reconnect alone does not clear this stop state. Retry only transport timeout/transient server failures within budget. Do not amplify a rejected upload into an unbounded retry loop.

### Worker and transport safety

Reuse the existing out-of-tree endpoint/CA/credential configuration boundaries and time-validity gating. Unconfigured endpoint, empty CA, missing credential, or unsynchronized time is explicitly unavailable, not a fake successful upload or a reason to disable TLS verification.

The owned asynchronous debug worker cannot run synchronous HTTP from acquisition, BLE callbacks, or the main-loop diagnostic call. Control and archive readiness take priority; debug is deferred while they have in-flight transport work. Existing control/telemetry workers must defer new starts while debug owns its bounded transport slot, so transport tasks do not enter the VFS domain concurrently through the new path. Cancellation retains the busy/ownership marker until the worker is actually quiescent. Include debug pending/teardown state in the existing filesystem transport exclusion.

Do not start filesystem work while a debug HTTP task is active. Audit all reachable storage paths, including media migration and maintenance/profile operations, not only the existing archive storage slice. An operation that cannot acquire safe filesystem ownership reports bounded busy/deferred state rather than performing I/O concurrently. Do not couple acquisition polling or calibration expiry to debug completion. Honor existing shared authorizer quotas; routine browser debug polling is paused during active calibration/control operations and device debug upload is never a tight poll.

### Persistence and query

Use a new SQLite debug table/migration, separate from research/telemetry/control journals. Current migrations end at `007_device_ble_identity.sql`; choose the next free filename again during implementation rather than reserving a stale number.

Store server-generated non-reusing monotonic ingestion row IDs, device/boot/event identity, canonical event content, and first receipt time. Duplicate retries do not refresh receipt time or create new history rows. `after`/`next_after` are canonical positive decimal server ingestion cursors within SQLite's signed-int64 range, with `0` allowed as the initial cursor; they are not JavaScript Numbers. Source uint64 sequences remain canonical text fields rather than overflowing SQLite integers. Ordering across boots uses server ingestion order while source sequence/uptime remain visible.

Apply per-device/global row caps atomically on insert and reject invalid pagination/filter bounds. Expired history is excluded from reads; bounded pruning removes expired/over-cap debug rows without touching mandatory telemetry or command evidence. Expose retention truncation/cursor gaps using per-device prune/expiry watermarks; gaps in a global row-ID sequence alone do not prove missing events for one device. These are logical retention limits, not a promise that SQLite/WAL filesystem bytes instantly shrink; no automatic whole-database VACUUM or unrelated database maintenance.

## 8. Production website behavior

Integrate into `app/devices/[id].tsx` and the real `WifiSettingsDialog`; do not replace them with an embedded manual helper page.

- One page-scoped BLE owner creates the client and distributes connection state, identity result, busy state, and diagnostic subscriptions to Wi-Fi, calibration, and debug consumers.
- Preserve operation-specific calibration deadlines and mutation semantics. Apply 30 seconds/attempt and at most 3 total attempts only to GET_DEVICE_ID, GET_STATUS, SCAN_WIFI. Genuine timeout is retryable only after safe write completion; decoder/device rejection, disconnect, unfinished/failed write, and partial-write failure do not trigger a resend. CONNECT/save/calibration remain single-send. Fast replies complete immediately, elapsed values reflect actual monotonic measurements, and progress notifications do not extend deadlines or become terminal success.
- Preserve valid hidden/empty scan-row validation/counting before filtering selectable empty SSIDs. Keep nonempty SSIDs literal and malformed data fatal.
- Disconnect, route/device change, and disposal invalidate generations and stale async callbacks. Clear passwords at submission and disconnect; late completion must not clear a newer session's password or replace its feedback. Dialog close must have an explicit owner-consistent lifecycle, not orphan its GATT client.
- Add separate Firmware debug and Client trace views. Firmware entries label USB/BLE/server source as applicable, device/boot/build, level/module/code, real duration, sequence, cursor gap, receipt time, and retention/loss limitations. A browser trace never appears as firmware evidence.
- BLE polling runs only while the debug view is visible, connected, identity-matched, and the shared session is idle; no overlapping request or polling during a mutating/calibration operation. HTTPS polling is similarly visibility/busy-aware, with a minimum 5-second interval and no hidden-tab loop.
- Source switching is explicit. Unsupported firmware, absent BLE, missing operator session, unavailable backend, or unconfigured HTTPS renders the actual unavailable reason. No mock, simulator, stale other-device data, or silent client-trace substitution.
- Local level selection is available over the owned BLE session; remote HTTPS view is read-only in this increment. Closing the view stops polling, not an unrelated in-flight MCU mutation.

## 9. Configuration ownership

Firmware debug policy is typed and instance-owned, initialized from build configuration. Put supported defaults/validation and runtime-level behavior in `docs/TASK9_Customization.md`. Production remains INFO by default; an explicitly named full-app debug environment inherits the real production source/configuration and selects DEBUG, rather than booting the restricted diagnostic main.

Keep secret-bearing endpoint/CA/token inputs in their existing reviewed external configuration paths. Expose only ready/unavailable reasons, not values. Backend retention/cap policy is a validated service option with the defaults in section 4; it does not mutate existing global authentication or ingest policy. No new settings storage format or AppConfig schema bump is required for volatile log level.

## 10. Acceptance and evidence gates

Implementation requires all relevant tests/builds against the current dirty tree, with exact commands, counts, source identity, and artifact hashes. Historical helper counts are not new production results.

1. Cross-language debug-v1 contract: actual C++ serializer/parser, TS decoder, Go validator, registry and size/uint64/privacy/cursor fixtures agree; invalid cases are rejected consistently.
2. Native firmware: overflow, independent cursors, concurrent producer/snapshot safety, nonblocking sinks, no recursive logging, boot/sequence semantics, volatile level changes, scan outcomes, real GATT-expiry cleanup, bounded retry/loss and immutable snapshot ACK matching.
3. Actual app composition: debug does not steal acquisition sequence IDs, alter calibration lease/owner, consume mandatory pending records, block acquisition, bypass TLS/time gating, or release VFS exclusion before worker cleanup. Inspect startup/persistence paths and no-passkey effective configuration/linked image.
4. Frontend: generation races, password privacy, shared-client ownership, mixed raw scan rows, progress/decoder ordering, late responses, exact retry allowlist, one-send mutations, source/identity separation, polling cancellation, unsupported firmware, and server cursor precision.
5. Go: isolated migration/store/handler/router/auth tests; malformed/oversized/duplicate-key input, wrong-device and unauthorized access, transactional dedup/conflict, retention/caps, pagination, and auth/rate-limit interaction. Run focused tests, full unit suite, race checks and vet.
6. Run `npm test -- --run`, `npm run typecheck`, `npm run export:web`, relevant Python/firmware host and native suites, and serial PlatformIO builds for affected production/SD/HTTPS/debug and diagnostic environments. Do not run shared `.pio` builds concurrently.
7. Exercise the real website/backend integration against an isolated test database and the debug codec fixtures. Label injected fixtures TEST, not physical device output. A browser chooser/radio result is HUMAN_ACTION_REQUIRED, not established by host tests or page render.
8. Produce staged full-app BIN/ELF/map and web artifacts plus source/contract checksums and a startup side-effect report. No automatic activation of a new server or replacement of the accepted helper.
9. Independent review must verify PRD/TSD/safety compatibility and returned evidence before physical permission is requested. Overall Task 9 stays HOLD.

### Physical acceptance, separately authorized

Full application startup can initialize sensors, actuators, persistence, and media even when the operator intends only BLE debugging. A source/startup audit and explicit permitted hardware/persistence scope are prerequisites; app-only flashing is not permission for every subsequent startup effect.

When permitted, use an exclusive serial-monitor subagent with verified PID/port/readiness, a bounded observation window, passive allowlisted capture, and one operator reproduction at a time. Read-only monitor silence is inconclusive. Do not reset, reconnect, flash, retry physical operations, or broaden permissions on a failure marker.

Then separately establish flashed/readback image identity, production boot readiness, local USB event visibility, BLE identity/status/scan and idle reconnect behavior, permitted Wi-Fi/persistence behavior, and actual authenticated HTTPS upload/query. Sensor/actuator, SD, calibration mutation, and endurance gates retain their own permissions. Report exactly the exercised phases and leave other gates NOT_RUN/HUMAN_ACTION_REQUIRED, not PASS.

## 11. Execution handoff and approval state

Main agent remains orchestrator. After written-spec approval, prepare a written implementation plan for owner review and execution-method selection. Implementation plus focused tests belongs to the execution agent, using the explicitly verified `gpt-6-luna` / `openai-codex` / `high` route; no silent fallback. Independent contract/backend/frontend work can be parallelized only after shared contracts and disjoint file ownership are established. The execution agent may decompose further; main-agent supervision/monitoring and independent evidence review remain required.

This document records approved intent and technical choices. The owner approved the written plan at `docs/superpowers/plans/2026-10-21-sigap-production-debug-implementation.md` on 2026-10-22 WIB, authorizing source/host implementation through the preserved delegated execution method. Toolchain installation, physical actions, deployment, commit/push, and Task 9 acceptance remain separately gated.
