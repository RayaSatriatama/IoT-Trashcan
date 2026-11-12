# SIGAP Production BLE and Firmware Debug Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use subagent-driven-development with the preserved execution-agent method. Steps use checkbox (`- [ ]`) syntax for tracking. Owner approved source/host execution through `lanjutkan` on 2026-10-22 WIB. Physical actions, deployment, Git writes, and toolchain installation retain their separate permission boundaries.

**Goal:** Integrate the accepted BLE/Wi-Fi behavior into the real website/full firmware and expose actual firmware debug events over USB, BLE, and authenticated HTTPS.

**Architecture:** One typed debug-v1 contract feeds independent bounded sinks and a separate backend debug store. One page-scoped BLE owner serves Wi-Fi, calibration, and debug; the existing firmware calibration controller remains authoritative. Optional debug work cannot block acquisition, bypass filesystem/TLS ownership, or modify mandatory archive semantics.

**Tech Stack:** Existing C++17/Arduino/PlatformIO, espressif32@7.1.3, NimBLE-Arduino@2.5.1; Go directive 1.27.1 with modernc.org/sqlite v1.59.0; Expo ~57.0.23, React 19.2.3, TypeScript ~6.0.3, Vitest ^3.2.7. These are inspected manifest versions, not a claim that every tool is currently executable.

**Spec:** `docs/superpowers/specs/2026-10-21-sigap-production-debug-design.md`, written specification approved by the owner's `lanjutkan` on 2026-10-21.

**Root:** `/mnt/nvme0n1p4/Projects/Programming_Projects/full-integration-hil-validation`.
**Baseline HEAD:** `0257b9e5cc08b142348f85f6da184842c09ab420`, branch `feat/full-integration-hil-validation`; preserve the existing dirty tree.
**Execution method already supplied:** main agent orchestrates; execution agent implements each owned slice with its tests and proposes useful decomposition; independent review follows. Do not ask the owner to select the method again.

## Global Constraints

- No BLE passkey, setup-secret gate, or new BLE application authentication for this prototype. A matching self-reported identity remains a routing check, not authentication.
- Do not remove HTTP authorization, device credential checks, operator permissions, CSRF protection on existing mutations, or TLS verification.
- No automatic flash, reboot, partition modification, erase, provisioning save, profile/calibration COMMIT, SD write, heater activation, sensor/actuator physical test, firewall change, or public deployment.
- No commit, staging, push, merge, reset, stash, or cleanup of unrelated work. Review checkpoints replace the skill's usual commit steps.
- No Git write operation is authorized for task-related work either; do not stage/commit this feature without a separate owner request.
- Reuse the pinned toolchain/dependencies. Do not upgrade the SDK, replace the web framework, or add a logging platform to deliver this feature.
- Do not change telemetry-v3 schemas, immutable pending bodies, ACK/reclaim rules, research evidence, FIS/safety ownership, or acquisition scheduling to accommodate debug logs.
- Do not copy diagnostic-image NVS erase guards or DHCP-cache wrappers into the full application.
- RAM event ring: 64 records; maximum 64 bytes per stored record, checked at compile time. Ring record storage: at most 4096 bytes, excluding service state and sink buffers.
- BLE page: at most one event; complete response within the existing 512-byte response boundary. Preserve 20-byte ATT packets and 128-chunk firmware limits.
- HTTPS snapshot: at most 8 records; at most 4096 serialized request bytes; pack fewer if needed. One outstanding debug request, minimum 5-second upload cadence, 5-second request deadline, at most 3 attempts for the same frozen snapshot, initial 5-second exponential backoff capped at 60 seconds.
- Browser client trace: existing 60-event in-memory limit. Browser firmware view: at most 200 displayed events. Backend query page: at most 100 events.
- Backend retention: 24 hours of received debug history, at most 2000 events/device and 20000 events globally.
- The production default is INFO; an explicit full-application debug build defaults to DEBUG. Runtime USB/BLE level selection is volatile and bounded to ERROR, WARN, INFO, DEBUG.
- GET_DEVICE_ID, GET_STATUS, SCAN_WIFI: 30 seconds/attempt, at most 3 total attempts after a genuinely retryable timeout. GET_DEBUG: 30 seconds, one attempt. CONNECT/save/calibration/SET_DEBUG_LEVEL remain single-send. Preserve existing calibration deadlines rather than globally changing requestTimeoutMs.
- Never include frames, response bodies, SSIDs, passwords, tokens, setup secrets, NVS values, bond data, HTTP headers, secret-bearing URLs, or free-form error text in debug records.
- One actual project command per terminal call with its own exit code. No concealed shell chains or subprocess test batches. Native test harness compilation/execution is part of the test itself, not permission to hide unrelated project commands.
- Keep edited text files below 1500 lines. Measure before growth; request permission before expanding an oversized file or restructuring it to evade the limit.
- No silent fallback, fake live data, automatic chooser/reconnect, deployment, or hardware acceptance. Overall Task 9 remains HOLD.

## Review Focus

1. RF1: New debug enum values must not satisfy the existing ordinal `calibration_command()` test and accidentally reach calibration dispatch; actual debug reads/level changes must not renew a calibration lease. Owned by Task 3.
2. RF2: A cancelled HTTPS task may still own TLS/VFS resources. Storage/profile/media operations remain excluded until real quiescence, while acquisition and controller expiry continue. Owned by Task 6.
3. RF3: Disconnect, route/device change, close/reopen, or late completion must not clear a newer password, re-enable an old session, or release another calibration view's activity state. Owned by Task 5.
4. RF4: Reading/uploading debug events, a slow USB host, or a full ring must not recursively generate logs, consume mandatory sample records, or block control work. Owned by Tasks 2 and 7.
5. RF5: Large uint64 values, multiple boots/devices, duplicates, retention pruning, and gaps in global SQLite row IDs must not round IDs or create fictitious per-device loss. Owned by Tasks 1, 4, and 8.

## Prerequisites and evidence rules

- Planning inspection found g++, node, npm, and `/home/raya/.platformio/penv/bin/pio`. `pio test --help` verified `-d`, `-e`, and `-f` flags.
- Go was not on the current PATH or in the checked standard/local SDK locations. Execution must resolve an existing compatible toolchain or obtain explicit setup permission before claiming any Go check. Do not silently download a toolchain, lower go.mod, or replace Go checks with a plausible report.
- There is no root `.venv/bin/python`; pytest was absent in the inspected Python environments. New host probes follow the repository's existing unittest style and use `/home/raya/.platformio/penv/bin/python`; no pytest installation is required by this plan. Audit existing Python tests before claiming complete unittest discovery coverage.
- All command results in later tasks are expected outcomes, not results observed during plan creation. Capture RED for a precise assertion or missing new interface, then GREEN for the same check.
- Before delegation, load the Hermes/dispatch workflow and verify `delegation.model`, `delegation.provider`, and `delegation.reasoning_effort` with the three existing `hermes config get` commands. Required route: gpt-6-luna / openai-codex / high. On mismatch or quota failure, preserve partial work and report the blocker; no config rewrite or substitute model.

```text
hermes config get delegation.model
hermes config get delegation.provider
hermes config get delegation.reasoning_effort
```
- A native Hermes child cannot recursively call delegate_task. Let the execution agent propose decomposition and have the lead dispatch its requested disjoint slices; do not promise unsupported recursive delegation. Keep one active firmware composition owner.
- Activate and verify supervision at minute 20 and every 20 minutes while subagents remain active; stop it when none remain. No hardware-monitor process is needed for this source-only phase.

## File structure and dependency order

New paths below are proposed outputs. Existing paths were inspected; line references describe this baseline and must be refreshed before edits.

| Task | Unit and principal files | Dependency |
| --- | --- | --- |
| 1 | `contracts/debug/v1/`, new `firmware/lib/debug/src/debug/{types,codec,registry}.h`, `lib/debug/{types,codec}.ts`, `internal/devicedebug/contract.go` | Toolchain prerequisites for cross-language verification |
| 2 | New bounded `debug/service`, `debug/usb_sink`, `debug/commands`; existing maintenance parser/router and native flags | 1 |
| 3 | Existing provisioning BLE session/service, new `ble_service_queue.h`, TS BLE client/protocol debug extension | 1, 2 |
| 4 | New Go debug store/HTTP service, migration, existing HTTP router/composition | 1 |
| 5 | New page-scoped BLE/Wi-Fi controllers; existing Wi-Fi dialog, calibration activity reporting, device route | 3 |
| 6 | New debug HTTP worker/transport and runtime I/O gate; existing app/network/storage mutation boundaries | 2, 3, 4 |
| 7 | Actual firmware operation instrumentation, sample observer fan-out, full-app debug build | 3, 6 |
| 8 | New real website debug view, authenticated API and cancellable polling controller | 4, 5, 7 |
| 9 | Actual-producer-to-real-handler/web integration and staged artifacts | 1-8 |
| 10 | Independent review, runbook/evidence handoff, physical permission request preparation | 9 |

Tasks 2 and 4 may run in parallel after Task 1 freezes the contract. Task 5 may run alongside Task 6 with separate ownership. Tasks 3/6/7 touch firmware composition and run serially. Task 8 is not started against guessed interfaces. Shared `.pio` output is never built concurrently.

Inspected current sizes: BLE client 544 lines, client tests 662, protocol 325, firmware app cpp 707, app header 356, maintenance runtime cpp 997, BLE cpp 891, existing maintenance native test 1172. Put new debug suites in separate test directories rather than growing the maintenance suite past its limit.

---

### Task 1: Freeze a small cross-language debug-v1 contract

**Files:**
- Create: `contracts/debug/v1/registry.json`, `wire.md`, `fixtures/{events.json,ble.txt,http.json,ack.json,invalid.json}`.
- Create: `firmware/lib/debug/src/debug/types.h`, `registry.h`, `codec.h`, `codec.cpp`.
- Create: `lib/debug/types.ts`, `lib/debug/codec.ts`, `lib/debug/codec.test.ts`.
- Create: `internal/devicedebug/contract.go`, `contract_test.go`.
- Create: `firmware/test/host/debug_contract/probe.cpp`, `tests/test_debug_contract.py`.
- Read: existing `acquisition_types.h`, `boot_identity.h`, `sequence.h`, `lib/ble/protocol.ts:59-151`, and frozen telemetry identity encoders. Do not modify telemetry-v3.

**Interfaces:**
- C++ namespace `sigap::debug`: `enum class Level : uint8_t { Error=0, Warn=1, Info=2, Debug=3 }`; `Module` numeric Runtime=1, Ble=2, Wifi=3, Storage=4, Acquisition=5, Calibration=6, Https=7. JSON/command level names remain uppercase ERROR/WARN/INFO/DEBUG.
- `EventRecord`: uint64 sequence/monotonic_us/related_sequence; uint32 request_id/duration_us; int32 arg0/arg1; uint16 event_code; Level level; Module module; uint8 presence bits for request_id/related_sequence. `static_assert(sizeof(EventRecord) <= 64U)`.
- `EventInput` has those fields except sequence; the service allocates its own sequence starting at 1. An independent `acquisition::SequenceAllocator` instance is permitted; never share boot_identity_.sequence.
- `IEventSink { virtual bool emit(const EventInput&) noexcept=0; }` belongs to types.h now; Task 2's DebugService implements it and Task 7 consumes it.
- `Cursor { acquisition::BootId boot; uint64_t after; }`; after=0 means before the first event. Missing boot identity is an explicit unavailable error, not a made-up boot.
- `Meta`: boot, build identity bounded to 96 printable ASCII bytes, effective Level, oldest/latest sequences, overwritten/delivery_lost/usb_lost counters, captured_monotonic_us, and bool sequence_exhausted. Event sequence 0 is reserved for an initial cursor/empty bounds, not an emitted event.
- `Page`: boot, level, oldest, next_after, overwritten, gap, boot_changed, `bool has_event`, EventRecord event. No build field on PAGE.
- `BatchSnapshot`: Meta meta, bool gap/boot_changed, `std::array<EventRecord,8> events`, size_t count. Task 2 captures all metadata/records under one lock; Task 6 formats and sends only a fitting prefix after unlocking.
- `bool encode_meta(const Meta&, char*, size_t, size_t&) noexcept`; `bool encode_page(const Page&, char*, size_t, size_t&) noexcept`; `bool encode_event_json(const EventRecord&, char*, size_t, size_t&) noexcept`.
- C++ `UploadAck` contains schema/status, exact bounded device identity (64 bytes, matching the inspected kMaxDeviceControlIdBytes), BootId, first/last sequence, stored/deduplicated counts and 32-byte request digest. `bool decode_upload_ack(std::string_view, UploadAck&) noexcept` rejects extra/missing fields and is tested with the same Go ACK fixture.
- TS exports matching DebugEvent/DebugMeta/DebugPage/DebugLevel/DebugCursor and `decodeDebugResponse(text: string): DebugResponse`, a discriminated union with kind meta/page/level. uint64 values remain canonical strings, optional values null.
- Go exports DebugEvent, Upload, UploadAck, HistoryPage and `DecodeUpload(raw []byte) (Upload, error)`. Invalid schema/field/code/range errors have fixed public codes; no raw error/body output.

**Wire choices to document/freeze:**

Commands use existing terminal LF encoder convention, removed before F1 framing:

```text
GET_DEBUG META
GET_DEBUG PAGE <canonical-boot-id> <canonical-after-sequence>
SET_DEBUG_LEVEL DEBUG
```

Angle-bracket tokens above define typed wire positions, not literal command input. Actual fixtures contain complete values from the probe. No new GATT UUID or change to F1/R1 framing.

```text
DBG1|M|boot|build_ascii_as_lower_hex|level_number|oldest|latest|overwritten|delivery_lost|usb_lost|captured_us|exhausted_bit
DBG1|P|boot|level_number|oldest|next_after|overwritten|gap_bit|boot_changed_bit|event_tuple_or_dash
DBG1|L|level_number
```

The event tuple is comma-separated sequence, monotonic_us, level, module, event_code, request_id-or-dash, duration_us, arg0, arg1, related_sequence-or-dash. Validate exact arity, canonical numbers, registered module/code pair, flags, boot identity and strict ASCII hex decoding. Numeric optional fields use `-` only in their specified positions.

Registry event IDs: RuntimeBoot=1, RuntimeStage=2, RuntimeLoopHealth=3; BleConnected=10, BleDisconnected=11, BleRequestStarted=12, BleRequestProgress=13, BleRequestCompleted=14, BleRequestRejected=15, BleIdleExpired=16; WifiScanStarted=20, WifiScanPending=21, WifiScanReady=22, WifiScanRateLimited=23, WifiScanFailed=24, WifiLinkTransition=25; StorageState=30, StorageQueue=31, StorageOutcome=32; AcquisitionQuality=40, AcquisitionCounters=41, ProcessingAvailability=42; CalibrationOutcome=50, CalibrationOwner=51, ProfileState=52, ModelState=53; HttpsReadiness=60, HttpsOutcome=61, HttpsQueue=62. Give arg0/arg1 an exact enum/count meaning for every event in registry.json; they never carry a secret, arbitrary pointer, or raw sensor value.

Upload has schema_version=1, device_id, boot_id, build_id, level, captured_monotonic_us, oldest_sequence, overwritten, delivery_lost, events. JSON level uses uppercase names; module/event_code use registry numeric IDs. uint64 fields use decimal strings; optional fields use null. Reject unknown keys, invalid UTF-8, duplicate keys, depth beyond 8, trailing JSON, device identity beyond the existing 64-byte control-plane bound, build beyond 96 printable ASCII bytes, event serialization beyond 512 bytes, body beyond 4096 bytes, or an event array outside 1..8. Empty rings do not generate HTTP uploads. Require strictly increasing positive event sequences, event time no later than captured time, one boot per envelope, and exact authorized device identity.

ACK has schema_version=1, status=DURABLE, device_id, boot_id, first_sequence, last_sequence, stored_count, deduplicated_count, request_sha256. The hash is lowercase SHA-256 of exact request bytes. It is not an archive ACK. HTTP 200 plus an exact matching valid ACK is success; 202/empty/truncated/wrong-identity/wrong-hash bodies are not success.

- [ ] Step 1: Write actual-codec boundary tests. The Python unittest compiles probe.cpp against codec.cpp, captures generated frames/JSON in a scratch directory, and compares them with committed fixtures; TS/Go decode those same fixture bytes. Use unittest/subprocess in the existing host-test style, not a replacement mock serializer.

```ts
import { expect, it } from 'vitest';
import { decodeDebugResponse } from './codec';
it('keeps uint64 values exact across PAGE decoding', () => {
  const text = 'DBG1|P|00000000000000000000000000000001|2|1|18446744073709551614|0|0|0|18446744073709551614,9007199254740993,2,1,1,-,0,0,0,-';
  const page = decodeDebugResponse(text);
  expect(page.kind).toBe('page');
  if (page.kind !== 'page') throw new Error('wrong test response');
  expect(page.event?.monotonic_us).toBe('9007199254740993');
});
```

- [ ] Step 2: Run each RED check separately: `npm exec -- vitest run lib/debug/codec.test.ts`; `/home/raya/.platformio/penv/bin/python -m unittest discover -s tests -p test_debug_contract.py -v`; `go test ./internal/devicedebug -run TestDebugContract -count=1`. Go remains NOT_RUN until its toolchain prerequisite is resolved.
- [ ] Step 3: Implement fixed-buffer writers and strict validators. Freeze fixture bytes generated by actual C++; do not manually invent expected firmware output. Register optional-field presence consistently; validate empty META/PAGE, boot change, malformed/duplicate fields, wrong enum/module pairs, leading zeros, overflow, INT32 limits, and wrong ACK hashes.
- [ ] Step 4: Execute the same three checks GREEN. Add current F1/R1 encoder framing of both payloads at IDs 9/10 and the largest supported ID/body combinations. Unsupported headers/chunk counts must fail before writes; never wrap/recycle IDs or enlarge ATT frames. Recompute conservative maximum-width META/PAGE text sizes from the exact grammar, including the exhaustion flag, and verify actual producers rather than treating a design calculation as codec evidence.
- [ ] Step 5: Review exact registry/schema/fixture hashes before downstream work. No staging or commit. Deliverable: independently testable versioned encoders/decoders and frozen fixture corpus.

### Task 2: Bounded event service, independent cursors, and USB access

**Files:**
- Create: `firmware/lib/debug/src/debug/{service.h,service.cpp,usb_sink.h,usb_sink.cpp,commands.h,commands.cpp}`.
- Modify: `firmware/lib/maintenance/src/maintenance/{maintenance.h,maintenance.cpp,runtime.h,runtime.cpp}` for explicit debug commands/handler binding only.
- Modify: `firmware/platformio.ini` include paths and native test integration; preserve all existing security/diagnostic flags.
- Modify: `docs/TASK9_Customization.md` for the existing maintenance response bound and new typed debug policy only.
- Create: `firmware/test/native/test_debug_service/test_debug_service.cpp`, `firmware/test/native/test_debug_usb/test_debug_usb.cpp`.
- Create: `firmware/test/host/debug_concurrency/probe.cpp`, `tests/test_debug_concurrency.py`.

**Interfaces:**
- `IDebugLock { virtual void lock() noexcept=0; virtual void unlock() noexcept=0; }`; Arduino uses a short fixed-copy critical-section adapter; host concurrency probe uses a real std::mutex adapter, not a no-op fake.
- `DebugService` implements Task 1's IEventSink. `DebugService(const acquisition::BootId&, std::string_view build, Level, IDebugLock&) noexcept`; `bool emit(const EventInput&) noexcept`; `Meta meta(uint64_t now_us) const noexcept`; `Page read(const Cursor&) const noexcept`; `bool set_level(Level) noexcept`; `void note_delivery_loss(uint64_t count) noexcept`; `void note_usb_loss(uint64_t count) noexcept`.
- `bool capture(const Cursor&, size_t max_events, uint64_t now_us, BatchSnapshot&) const noexcept` copies one internally consistent snapshot, at most 8 events, under the same short lock. It never formats JSON or advances a sink cursor; empty/boot-change states remain explicit.
- Service holds 64 records and a distinct sequence allocator starting at 1. Reject invalid input/identity without allocating a sequence. Exhaustion does not wrap. Test allocator exhaustion with the existing SequenceAllocator(UINT64_MAX) seam rather than billions of emits or a production reset backdoor.
- `IUsbDebugWriter`: `size_t writable() noexcept`, `size_t write(const char*, size_t) noexcept`; `UsbDebugSink(DebugService&, IUsbDebugWriter&)`; `void poll(uint64_t now_us, size_t byte_budget) noexcept`. Keep one bounded pending line/offset and an independent cursor.
- New maintenance `IDebugCommandHandler` forwards DebugMeta/DebugPage/DebugLevel to debug::CommandBackend. Bind through explicit `CommandRouter::set_debug_commands(IDebugCommandHandler*)`; absent binding yields DEBUG_UNAVAILABLE, not fabricated data.
- `CommandBackend(DebugService&) noexcept` exposes `bool meta(uint64_t now_us, char*, size_t, size_t&) noexcept`, `bool page(const Cursor&, char*, size_t, size_t&) noexcept`, and `bool set_level(Level, char*, size_t, size_t&) noexcept`. USB and BLE adapters map their parsed arguments into these same typed methods, not separate mutable log-level state.
- USB grammar: `debug meta`, `debug page BOOT AFTER`, `debug level ERROR|WARN|INFO|DEBUG`. Existing target/field/value bounds accommodate typed arguments. Preserve the actual 384-byte maintenance response constant; do not enlarge it for debug.

- [ ] Step 1: Write native tests for record overflow, independent cursors, gap/boot change, filtered/invalid records, level reset, and a host that accepts zero bytes. Define the helper boot as a local BootId with present=true, all bytes zero except bytes[15]=1; no physical identity is implied.

```cpp
void test_observing_the_ring_does_not_consume_other_cursors() {
  acquisition::BootId boot{};
  boot.present = true;
  boot.bytes[15] = 1U;
  TestLock lock;
  debug::DebugService service(boot, "TEST", debug::Level::Info, lock);
  debug::EventInput input{};
  input.module = debug::Module::Runtime;
  input.event_code = 1U;
  input.level = debug::Level::Info;
  TEST_ASSERT_TRUE(service.emit(input));
  const debug::Cursor cursor{boot, 0U};
  TEST_ASSERT_TRUE(service.read(cursor).has_event);
  TEST_ASSERT_TRUE(service.read(cursor).has_event);
}
```

TestLock is the suite-local IDebugLock adapter with lock/unlock counters; use the real mutex adapter in the separate concurrent probe. Register every test in the suite's Unity main.

- [ ] Step 2: RED: `/home/raya/.platformio/penv/bin/pio test -d firmware -e native -f test_debug_service`; then `/home/raya/.platformio/penv/bin/pio test -d firmware -e native -f test_debug_usb`; then `/home/raya/.platformio/penv/bin/python -m unittest discover -s tests -p test_debug_concurrency.py -v`, each in its own call.
- [ ] Step 3: Implement bounded append/read/copy and zero-I/O critical sections. USB lines begin `SIGAP_DEBUG ` and encode registered metadata only. Retry a partial host write from its pending offset, bounded by writable bytes and poll budget, never a flush loop. No sink operation emits a routine debug event about itself. Add repeated-condition counters without an unbounded map.
- [ ] Step 4: Implement parser/router forwarding and actual level acknowledgements; test no AppConfig/SecretStore/profile calls, no calibration lease renewal, invalid levels rejected, reconnect/default level behavior, and META/line size within the existing maintenance boundary. The source customization table's historical 240-byte wording must be corrected to the already-existing 384-byte source bound, with no protocol expansion.
- [ ] Step 5: Re-run the same GREEN checks plus existing test_maintenance, serially. Capture sizeof/service/sink buffers separately. Deliverable: native-tested service and USB adapter, not a claim of physical USB output.

### Task 3: Production BLE debug commands, serialized session access, and safe client deadlines

**Files:**
- Modify: `firmware/lib/provisioning/src/provisioning/{ble_provisioning.h,ble_provisioning.cpp}`.
- Create: `firmware/lib/provisioning/src/provisioning/ble_service_queue.h`.
- Create: `firmware/test/native/test_ble_debug/test_ble_debug.cpp`, `firmware/test/native/test_ble_service_queue/test_ble_service_queue.cpp`.
- Create: `firmware/test/host/ble_service/include/` focused Arduino/NimBLE shims, `firmware/test/host/ble_service/probe.cpp`, `tests/test_ble_service_lifecycle.py`.
- Modify: `lib/ble/{client.ts,protocol.ts}` and the existing focused client/protocol suites without replacing regressions.
- Create: `lib/ble/debug-client.test.ts`.

**Interfaces:**
- Extend BleCommandKind explicitly with DebugMeta/DebugPage/DebugLevel; add dedicated typed debug arguments, not calibration_arguments reuse.
- Session `set_debug_backend(debug::CommandBackend*)`; no binding returns explicit DEBUG_UNAVAILABLE. `bool consume_disconnect_request() noexcept` reports an idle-expiry closure once.
- Arduino service owns a 16-entry fixed callback queue of connect/write/disconnect events. Each write stores at most the existing 192-byte characteristic/command bound, connection epoch/handle, and length; normal F1 writes remain 20-byte ATT packets. Do not truncate or newly reject a previously supported unframed command merely because it is longer than one ATT packet; preserve it through the same existing parser and cover that case in the host fixture. Callback code copies/enqueues only; session receive/dispatch, main-loop polling, and actual calibration execution share one main-loop owner. Drain at most 4 callback events per poll; notify outside the queue lock. Account for the callback queue separately in the memory report.
- Queue overflow marks the session failed and schedules one real GATT disconnect; invalidate partial assembly and zero consumed/discarded payload storage. Do not silently drop a command and then retry a mutation. Existing callback public entry points remain, backed by this queue.
- Promote connection-handle tracking to production and validate epoch/handle before processing queued events. This removes the existing poll/callback session-state overlap rather than adding another unsynchronized debug path.
- TS BleResponse adds debug?: DebugResponse; client methods `debugMeta(): Promise<DebugMeta>`, `debugPage(cursor: DebugCursor): Promise<DebugPage>`, `setDebugLevel(level: DebugLevel): Promise<DebugLevel>`.
- Client options add `commandTimeoutMs?: Partial<Record<'GET_DEVICE_ID'|'GET_STATUS'|'SCAN_WIFI'|'GET_DEBUG', number>>`. Copy/validate finite positive entries. Unspecified entries retain existing requestTimeoutMs/defaults; calibration keeps its existing deadline and one-send behavior.

- [ ] Step 1: Add RED native tests that debug commands are not calibration commands, GET_DEBUG does not extend an active controller lease, stale queued writes cannot act on a new connection, overflow does not execute partial commands, and idle expiry leads to exactly one physical disconnect. Probe the actual Arduino service with SDK-shaped shims; a session-only fake cannot prove GATT closure.

```cpp
void test_debug_meta_is_not_routed_as_calibration() {
  Backend wifi;
  Clock clock;
  BleProvisioningSession session(wifi, clock);
  CalibrationBackend calibration;
  session.set_calibration_backend(&calibration);
  session.on_connect(0U);
  BleResponse response{};
  session.receive("GET_DEBUG META", response);
  TEST_ASSERT_TRUE(calibration.last_command.empty());
}
```

Use suite-local Backend/Clock/CalibrationBackend following the inspected test_ble_provisioning adapters; copy the complete tiny adapter definitions into the new suite, not undeclared cross-suite helpers.

- [ ] Step 2: RED: `/home/raya/.platformio/penv/bin/pio test -d firmware -e native -f test_ble_debug`; then `/home/raya/.platformio/penv/bin/pio test -d firmware -e native -f test_ble_service_queue`; then `/home/raya/.platformio/penv/bin/python -m unittest discover -s tests -p test_ble_service_lifecycle.py -v`, separately. TS RED: `npm exec -- vitest run lib/ble/debug-client.test.ts`.
- [ ] Step 3: Replace the ordinal `kind >= CalibrationCapabilities` predicate with explicit membership for all current calibration enum values. Preserve all existing calibration dispatch behavior, no-passkey properties, diagnostic guards, scan framing, and credential candidate policy. Map scan RateLimited to a distinct ERR SCAN_RATE_LIMITED instead of generic failure.
- [ ] Step 4: Implement queued main-loop service ownership, one-shot expiry disconnect, new commands and strict typed decoding. Do not rely on draining SESSION_TIMEOUT after logical disconnect; physical disconnect is authoritative. Debug get/set never calls calibration activity/renewal code.
- [ ] Step 5: Add per-command timer selection to requestAttempt, leaving requestCalibration on its existing timer. Preserve decoderRejected latching before write completion and the exact three-command retry allowlist. GET_DEBUG and SET_DEBUG_LEVEL each send once; wrong response kind rejects explicitly.

```ts
const productionClientOptions = {
  readOnlyMaxAttempts: 3,
  commandTimeoutMs: {
    GET_DEVICE_ID: 30_000, GET_STATUS: 30_000,
    SCAN_WIFI: 30_000, GET_DEBUG: 30_000,
  },
};
```

This is the option object later consumed by Task 5's owner; do not set global requestTimeoutMs=30000 and accidentally change calibration.

- [ ] Step 6: GREEN focused checks, existing BLE/native tests, and existing client/protocol tests. Cover ID9/ID10 LF framing, exact progress ACKs, decoder-before-write-completion ordering, old-response isolation, unfinished/failed writes, true read timeout retry limit, GET_DEBUG one attempt, and SET_DEBUG_LEVEL/calibration one-send. Deliverable: real codecs/service and host-tested lifecycle, radio NOT_RUN.

### Task 4: Authenticated debug persistence and history API

**Files:**
- Create: next free `migrations/008_device_debug.sql` only if 008 is still free; otherwise choose the next lexicographic migration and record its exact filename.
- Create: `internal/devicedebug/{store.go,http.go,store_test.go,http_test.go}`; consume contract.go from Task 1.
- Modify: `internal/http/router.go`, `cmd/sigap/main.go`, `internal/database/database_test.go` migration checks.
- Create: `internal/http/device_debug_test.go`.
- Read: `internal/devicecontrol/{auth.go,http.go}`, `internal/database/{database.go,migrate.go}`. Do not modify authorizer quotas or create live sessions.

**Interfaces:**
- `Policy { Retention time.Duration; PerDeviceLimit, GlobalLimit int }`, `DefaultPolicy()` returns 24h/2000/20000; validate positive retention/caps and global>=per-device.
- `NewStore(db *sql.DB, policy Policy) (*Store,error)`; `WithClock(func() time.Time) *Store`; `Ingest(ctx context.Context, upload Upload, exactBody []byte) (UploadAck,error)`; `Query(ctx context.Context, deviceID, after string, limit int) (HistoryPage,error)`.
- HTTPOptions carries the existing devicecontrol.OperatorAuthorizer/DeviceAuthorizer interfaces and AllowedOrigin. `NewHandler(*Store, HTTPOptions) http.Handler`.
- New `httpapi.WithDeviceDebug(*devicedebug.Store, devicedebug.HTTPOptions) RouterOption` installs the two spec routes. main passes the existing deviceControlAuth pointer, not a new independent authorizer.
- HistoryPage: events with server ingestion_id/received_at and source boot/build/level/captured time; next_after, oldest_available, retention_gap. All IDs remain validated strings.

- [ ] Step 1: Define isolated testStore using database.Open(filepath.Join(t.TempDir(),"debug.db")), seed a disposable location/device and digest, inject a fixed clock, and register db.Close cleanup. Add conflict rollback and exact dedup tests, not a real database migration.

```go
func TestMissingAuthorizerFailsClosed(t *testing.T) {
    store, _, _ := testDebugStore(t)
    handler := NewHandler(store, HTTPOptions{})
    request := httptest.NewRequest(http.MethodGet, "/api/device-debug/events/TEST-DEVICE?after=0&limit=10", nil)
    response := httptest.NewRecorder()
    handler.ServeHTTP(response, request)
    if response.Code != http.StatusUnauthorized {
        t.Fatalf("status=%d", response.Code)
    }
}
```

`testDebugStore` is a new suite-local helper defined in this step, following the inspected devicecontrol testStore style. Import net/http, net/http/httptest, testing, database/sql, context, time and existing database package as required by the actual test bodies.

- [ ] Step 2: RED: `go test ./internal/devicedebug ./internal/http ./internal/database -count=1`. Toolchain resolution remains a prerequisite, not permission to alter go.mod.
- [ ] Step 3: Add AUTOINCREMENT ingestion ID, UNIQUE(device_id,boot_id,sequence), canonical event JSON, build identity and first receipt time; store uint64 source sequences as TEXT. Reject a changed build identity for the same retained event identity rather than rewriting provenance. Add a per-device pruning watermark table, bounded by registered device identities. Use one transaction for validation/conflict checks, insert/dedup, watermark updates and caps. Reject a batch larger than the configured per-device cap before insertion; prune older rows so the newly acknowledged batch fits. Never refresh duplicates' received_at.
- [ ] Step 4: Implement strict bounded decoding from Task 1 and HTTP 200 only after transaction commit. Return fixed errors for bad body/413 size/401 auth/403 origin/409 content conflict. ACK echoes the actual snapshot identity/range/count and exact request hash. Do not return raw JSON or SQL errors. No operator POST, level mutation, or browser log relay is added.
- [ ] Step 5: Implement cursor queries and bounded pruning. Distinguish per-device prune/expiry watermarks from holes caused by other devices' global IDs; expired rows are excluded even before physical deletion. Retention limits are logical, not automatic VACUUM. Test two devices, multiple boots, >2^53 source values, signed-int64 cursor bounds, expired duplicate semantics, and atomic row caps.
- [ ] Step 6: GREEN same tests, then `go test -race ./internal/devicedebug ./internal/http ./internal/database` and `go vet ./internal/devicedebug ./internal/http ./internal/database`. Assert real DatabaseAuthorizer nil/expired/revoked/wrong-device cases and shared quota behavior using test-only sessions. Deliverable: isolated real-handler/store integration, no operational database changes.

### Task 5: One production BLE owner and race-safe Wi-Fi UI

**Files:**
- Create: `lib/ble/{device-session.ts,device-session.test.ts,wifi-settings.ts,wifi-settings.test.ts}`.
- Modify: `components/device/WifiSettingsDialog.tsx:19-112`, `components/device/CalibrationWizard.tsx:9-21,170-249`, `app/devices/[id].tsx:49-65`.
- Extend: `tests/routes/detail.test.ts`; create `tests/device/session-view.test.tsx` for rendering the production adapters.

**Interfaces:**
- `PublicBleClient = Pick<BleProvisioningClient, keyof BleProvisioningClient>` is the structural public client surface, without the concrete class's private-member nominal constraint. `DeviceBleSession` constructor takes expected BLE identity and optional `createClient(options: BleProvisioningClientOptions): PublicBleClient`; production creates one BleProvisioningClient with Task 3's option object. Expose immutable connection/name/identity/busy/generation/controlActive/trace snapshots and subscribe/unsubscribe.
- `connect(): Promise<void>` is user initiated and verifies the expected identity; missing identity rejects before chooser. `close():void` invalidates BLE operation generation and closes GATT, but retains page-owned subscriptions and unrelated HTTPS activity. `dispose():void` invalidates the page, all leases/subscriptions, and its connection.
- `run<T>(owner:'wifi'|'calibration'|'debug', operation:(client:PublicBleClient)=>Promise<T>): Promise<T>` rejects overlap and superseded generations. Expose a structurally compatible calibration adapter implementing the existing BleCalibrationAdapter methods through run, not a second client.
- `beginControlActivity(source:'ble-calibration'|'https-calibration'): () => void` returns an idempotent release closure for a unique page-generation lease. Releasing an old lease cannot release another view's activity; BLE close does not release HTTPS activity. Debug polling is disabled while any lease is active, not only while a frame is pending.
- `WifiSettingsController(session)` owns Wi-Fi view state with explicit open/close/setPassword/selectSsid/connectBle/scan/submit/retrySaved methods and subscribe/getSnapshot. Password is UI-only state and never included in debug/trace DTOs.
- Wi-Fi dialog receives the shared session/controller, not a factory. Closing it clears its password/feedback but retains the page-owned connection; only explicit page disconnect/disposal closes shared GATT. Add a visible shared connection/disconnect control so this is not an orphaned connection.
- CalibrationWizard adds optional `onAcquireControlActivity?: () => (() => void)`, acquiring one lease while its busy/session-active boolean is true and releasing that same lease on terminal state/context disposal. Do not release/reacquire merely for a transition between two active states. Keep token/CSRF state in its existing scope; debug HTTPS has separate explicitly supplied in-memory authorization, not an automatic token copy.

- [ ] Step 1: Write pure controller tests with a typed fake client factory, deferred promises, and a real DeviceBleSession. Add construction-count, same-instance calibration/Wi-Fi, three-read policy, close/reopen, and RF3 cases. Define a local deferred helper returning promise/resolve/reject before its use.

```ts
it('does not clear a password entered after submit', async () => {
  const request = deferred<WifiStatus>();
  const session = new DeviceBleSession('0000000000000001', {
    createClient: (options) => fakeConnectedClient(options, { connectWifi: () => request.promise }),
  });
  await session.connect();
  const form = new WifiSettingsController(session);
  form.selectSsid('TEST-SSID');
  form.setPassword('test-only-old-password');
  const pending = form.submit();
  expect(form.getSnapshot().password).toBe('');
  form.setPassword('test-only-new-password');
  request.resolve({ state: 'CONNECTED', ssid: 'TEST-SSID' });
  await pending;
  expect(form.getSnapshot().password).toBe('test-only-new-password');
});
```

fakeConnectedClient implements PublicBleClient and invokes the supplied options' connection/identity callbacks; keep its complete test-only definition in the test file. It is not a radio implementation. Test both late resolve and reject after actual session generation change, not only a newer input in the same generation. Also acquire simultaneous BLE/HTTPS activity leases, release the BLE lease twice, close BLE, and assert the HTTPS lease still blocks polling until its own release.

- [ ] Step 2: RED: `npm exec -- vitest run lib/ble/device-session.test.ts lib/ble/wifi-settings.test.ts`.
- [ ] Step 3: Implement immediate password clearing before dispatch and no unscoped finally cleanup. Error feedback is separate from status/busy rendering. Route/device changes dispose old owner; deliberate reconnect accounts for synchronous old-session close callbacks without allowing unexpected disconnect to rebase work.
- [ ] Step 4: Replace both inspected new BleProvisioningClient constructors in the route/dialog with the single owner. Forward all existing calibration adapter methods, keep identity check labels honest, and preserve explicit calibration sessionStorage behavior without ever storing tokens/passwords there. Add lease-based activity tracking for both calibration views, including terminal/unmount cleanup; route disposal calls dispose(), not just the BLE-only close().
- [ ] Step 5: GREEN controller tests and route/render tests. Exercise production component props/runtime behavior, not only source-string assertions. Add hidden/empty scan rows, correct raw count, malformed data, no matching identity, true timeout-only retry, and concurrent operation rejection coverage. Run typecheck separately.
- [ ] Step 6: Deliver a working UI/controller integration using the existing Tamagui styles. Do not install a DOM test dependency or expand the design system; use existing pure controller tests, react-dom/server rendering, and Task 9's real browser smoke for lifecycle/render integration.

### Task 6: Bounded HTTPS debug worker and actual VFS ownership

**Files:**
- Create: `firmware/lib/debug/src/debug/{http_worker.h,http_worker.cpp,http_transport.h,http_transport.cpp,io_gate.h}`.
- Create: `firmware/test/native/test_debug_http/test_debug_http.cpp`, `firmware/test/native/test_debug_io_gate/test_debug_io_gate.cpp`.
- Modify: `firmware/src/app/{sigap_app.h,sigap_app.cpp,archive_runtime.h,device_control_executor.h}`, `firmware/lib/maintenance/src/maintenance/{runtime.h,runtime.cpp,runtime_calibration.cpp,model_runtime.h,model_runtime.cpp,ble_calibration_backend.h,ble_calibration_backend.cpp}` only where real persistence/admission paths need the same gate.
- Create: `firmware/test/host/debug_app/probe.cpp`, focused include shims, and `tests/test_debug_app_composition.py` that compile/exercise actual production translation units.
- Read: existing network/https_transport.h, device_control_http.h, network.h, `SigapApp::loop():650-698`, profile/model persistence call paths.

**Interfaces:**
- `IDebugHttpTransport`: `start(std::string_view exact_body, std::string_view token, uint32_t timeout_ms) noexcept -> network::TransportResult`; `poll(DebugHttpResult&) noexcept -> network::TransportResult`; `cancel() noexcept`; `quiescent() const noexcept -> bool`.
- DebugHttpResult carries status code, bounded <=512-byte ACK body, size, truncation flag; ACK decoder produces Task 1's UploadAck representation. Do not use telemetry_archive::Ack.
- `DebugHttpWorker` owns its endpoint/token lifetime, up to 8 copied events and <=4096 exact body bytes. `poll(uint64_t now_us, const DebugReadiness&) noexcept`; `cancel() noexcept`; `transport_pending() const noexcept`; status/loss accessors.
- DebugReadiness contains wifi/time/CA/config readiness, control_active, and other transport activity. No token, URL or payload is passed to log emitters.
- `RuntimeIoGate` is application-owned. `enum class IoOwner : uint8_t { ArchiveHttp, ControlHttp, DebugHttp, Filesystem }`; `try_acquire(IoOwner) noexcept -> bool`, `release(IoOwner) noexcept`, `active(IoOwner) const noexcept -> bool`. DebugHttp excludes every other owner; Filesystem excludes all HTTP owners. Preserve existing archive/control behavior but block their new starts while DebugHttp owns the slot. Ownership is released only after worker completion/teardown is observed, not cancel request. Gate transitions occur in the application loop, not late worker callbacks.
- Persistence operations check/acquire the gate before changing workflow state; on contention report bounded STORAGE_BUSY/deferred state without touching media. USB/BLE/HTTPS command admission must reject a new mutation before acquiring/changing calibration ownership while debug owns the transport slot, not fail halfway through Controller::execute. Debug does not start during calibration_controller_.active(). Acquisition, RAM handoffs and calibration_controller_.poll(now_us) never take this I/O gate.

- [ ] Step 1: Add a fake transport with independently controlled cancel_requested and quiescent states. Write RF2 and immutable-retry tests, including a completed HTTP response whose TLS cleanup has not yet quiesced.

```cpp
void test_cancel_is_not_vfs_quiescence() {
  debug::RuntimeIoGate gate;
  TEST_ASSERT_TRUE(gate.try_acquire(debug::IoOwner::DebugHttp));
  TEST_ASSERT_FALSE(gate.try_acquire(debug::IoOwner::Filesystem));
  TEST_ASSERT_TRUE(gate.active(debug::IoOwner::DebugHttp));
  gate.release(debug::IoOwner::DebugHttp);
  TEST_ASSERT_TRUE(gate.try_acquire(debug::IoOwner::Filesystem));
}
```

This pins the gate; the separate worker/app test must call cancel(), keep fake.quiescent=false, and verify it has not released that same gate. Releasing a fake gate directly is not evidence of actual cancellation correctness.

- [ ] Step 2: RED: run test_debug_io_gate, test_debug_http, and the production composition unittest separately.
- [ ] Step 3: Capture immutable envelope and exact body/hash once. Validate ACK identity/boot/first-last/count/hash before advancing; reject 202, oversized/truncated bodies and late ACKs. Keep byte-identical retries, respect 5-second cadence/deadline and three-attempt budget, explicitly account for final delivery loss. 401/schema/conflict stops survive ordinary Wi-Fi reconnect; transient failures follow bounded backoff.
- [ ] Step 4: Implement one owned asynchronous transport task using the existing installed SDK patterns; no synchronous HTTP in poll or BLE callbacks. Load/copy the existing application credential into worker-owned bounded storage while available, preserve zeroization and lifetime rules, and retain CA/time validation. No raw NVS/credential reads for tests. Unconfigured HTTPS compile targets remain unavailable at runtime.
- [ ] Step 5: Wire the I/O gate through all reachable filesystem paths, not only the existing archive slice: media migration, maintenance profile/model/config operations, storage probes and network cleanup. New starts during a debug slot are deferred; existing controller/archive requests take priority before debug begins. A new mutation encountering an active slot must report busy before state/media changes, not partially execute or auto-resend it.
- [ ] Step 6: GREEN worker and actual-app tests: keep an HTTP task pending across multiple app polls; assert no filesystem/probe/profile calls, continued acquisition/controller polls, no changed acquisition sequence allocator, and no release until quiescence. Test offline/time/CA/token readiness, restored network, cancellation races, body overflow packing, ring overwritten while a snapshot is in flight, and exact retry exhaustion. Deliverable: async host-tested integration with measured static buffers, not physical TLS success.

### Task 7: Instrument real module boundaries and add the full-app debug build

**Files:**
- Create: `firmware/src/app/{runtime_debug_observer.h,runtime_debug_observer.cpp}`.
- Modify: app sigap_app.h/cpp; provisioning BLE/Wi-Fi source; `firmware/lib/calibration/src/calibration/calibration_transport.h` and its existing implementation for actual execute/expiry outcomes; maintenance runtime/model files only at actual outcome boundaries.
- Modify: `firmware/src/app/{processing_runtime.h,archive_runtime.h}` for typed result/health notifications; do not replace normal record storage.
- Modify: `firmware/include/sigap/build/build_config.h`, `firmware/platformio.ini` with proposed `sigap_n16r8_debug` inheriting the real full application and a typed default level selection.
- Create: `firmware/test/native/test_runtime_debug_observer/test_runtime_debug_observer.cpp`; extend actual-app host probe and its tests.

**Interfaces:**
- Consume Task 1's `debug::IEventSink`, already implemented by DebugService in Task 2; do not define a second event-sink interface.
- RuntimeDebugObserver implements existing `acquisition::ISampleObserver`; constructor takes IEventSink and a reference to the existing downstream evidence observer. Its observe(const SampleRecord&) forwards that exact sample once, then records allowlisted quality/reason transition metadata without popping the archive handoff.
- Preserve the current chain: acquisition -> ProcessingRuntime -> evidence observer. Insert the new observer as the existing downstream fan-out, forwarding to the original evidence destination. Do not overwrite the downstream pointer and lose calibration evidence.
- Module insertion points take EventInput/numeric enums only. Optional sink absence in intentionally isolated diagnostic builds is explicit; the production composition must bind one service to every required area.
- Debug build uses the same production source and pins; no diagnostic main, NVS guard wrappers, new persistent config, or implicit activation.

- [ ] Step 1: Write RF4 tests that sample forwarding occurs exactly once even when the debug ring is full, no routine debug request/upload produces another event, and two consecutive real link/storage outcomes produce two transitions instead of the old one-shot latch behavior.

```cpp
void test_debug_observer_preserves_evidence_and_archive_ownership() {
  CountingSink sink;
  CountingSampleObserver evidence;
  RuntimeDebugObserver observer(sink, evidence);
  acquisition::SampleRecord sample{};
  observer.observe(sample);
  TEST_ASSERT_EQUAL_UINT32(1U, evidence.calls);
  TEST_ASSERT_EQUAL_PTR(&sample, evidence.last_seen);
}
```

Define CountingSink implementing IEventSink and CountingSampleObserver implementing ISampleObserver in the suite; their counters and last_seen pointer are initialized to zero/null. Add a separate real-handoff assertion proving the source record was not popped.

- [ ] Step 2: RED: native test_runtime_debug_observer and actual-app composition unittest, each separately.
- [ ] Step 3: Instrument spec section 5's seven areas with actual return/state evidence. Examples from inspected code: capture wifi_manager_.state() transitions; report config runtime readiness; observe archive_runtime_.health()/encode_failures()/storage_worker() rather than payload; use calibration_controller_.active()/owner() and execute result; forward existing sample quality/reason and processing availability, not invented raw/converted values.
- [ ] Step 4: Keep current commissioning markers, add registered start/pending/progress/terminal events and real duration/request IDs, and coalesce repeat conditions. Ordinary BLE request progress is sampled/aggregated; GET_DEBUG/level polling and debug upload success do not recursively fill the ring. SDK reset reason is reported only through APIs/enums actually supported by the pinned SDK.
- [ ] Step 5: Add validated INFO/DEBUG defaults, runtime reset behavior and policy docs. Re-run native observer/composition/USB/BLE suites. Build `sigap_n16r8_debug` without upload and verify it links the actual SigapApp rather than a diagnostic main. Record map/size deltas for ring, snapshot, codecs, task stack and total image; heap/timing headroom stays NOT_RUN without HIL.
- [ ] Step 6: Review all emit sites for payload/SSID/password/token/free-form error leakage and unchanged scientific/safety behavior. Deliverable: full-app instrumentation and build target, not an installed image.

### Task 8: Firmware debug view in the real website

**Files:**
- Create: `lib/api/debug.ts`, `lib/api/debug.test.ts`; consume existing OperatorAuthorization/Fetcher/ApiError types without changing auth policy.
- Create: `lib/debug/{poller.ts,poller.test.ts}`, `components/device/DeviceDebugPanel.tsx`, `tests/device/debug-panel.test.tsx`.
- Modify: `app/devices/[id].tsx`, reuse Task 5's shared session and activity reporting; add narrowly scoped render/poller tests.

**Interfaces:**
- `createDebugApi(auth: OperatorAuthorization, fetcher?: Fetcher, baseUrl?: string)` returns `history(deviceId:string, after:string, limit:number, signal?:AbortSignal):Promise<HistoryPage>`. Empty authorization rejects before fetch; no persistence or token logging.
- `DebugViewController` owns selected source ble/https, visibility, generation, AbortController and at most 200 entries. `setVisible(bool)`, `selectSource('ble'|'https')`, `refresh():Promise<void>`, `dispose():void`, `getSnapshot()/subscribe()`.
- BLE source uses session.run('debug', ...) with debugMeta/debugPage and cursor matching; HTTPS source uses createDebugApi. One pending refresh only; no automatic source fallback or chooser.
- Source identity keys are exact device + boot + event sequence; server receipt/cursor are additional metadata, never source event IDs. Boot change invalidates cached META before displaying the new page.
- Panel presents separate Firmware debug / Client trace views, level/module filter, real timings, source/build/boot, cursor/loss/retention notices, explicit refresh/disconnect and local level controls. Remote level is read-only. Token entry is explicit in-memory state cleared on device/session disposal, not inherited from another component.

- [ ] Step 1: Write tests for visibility/cancellation, source change during pending response, device change, boot change, unsupported firmware, missing operator session and exact large cursor strings. Use a suite-local abort-aware fake transport; no live-looking data in production.

```ts
it('requires operator authorization before fetching history', () => {
  const fetcher = vi.fn();
  expect(() => createDebugApi({ bearerToken: '' }, fetcher)).toThrow();
  expect(fetcher).not.toHaveBeenCalled();
});
```

- [ ] Step 2: RED: `npm exec -- vitest run lib/api/debug.test.ts lib/debug/poller.test.ts tests/device/debug-panel.test.tsx`.
- [ ] Step 3: Implement GET-only auth client and strict response decoding. Format uint64 strings without Number conversion. Duplicate retained events do not grow the view; global server ID gaps do not become firmware loss. Display server receipt separately from source uptime/capture age.
- [ ] Step 4: Implement one cancellable refresh loop at >=5 seconds while visible and not hidden/busy. Turning visibility off, switching source/device or disposal aborts the current fetch and forbids a later timer/fetch/append from its generation. Do not queue debug reads during active calibration or UI mutation. BLE initial META fetch and PAGE fetch are sequential under the shared owner.
- [ ] Step 5: Integrate the actual component and filters with existing Tamagui/AppButton styles. Clear only the local view on user action; do not clear MCU logs/NVS or imply remote level mutation. Render explicit unsupported/unavailable reasons, not stale other-device content or client trace as firmware data.
- [ ] Step 6: GREEN tests, full TS suite, typecheck and export in separate calls. Browser smoke remains Task 9; SSR/import tests alone do not prove loaded page lifecycle or actual Web Bluetooth.

### Task 9: Exercise the complete host integration and stage real artifacts

**Files:**
- Create: `internal/devicedebug/integration_test.go`, `tests/test_debug_end_to_end.py`, `tests/debug/production-flow.test.ts`.
- Extend: actual C++ debug probe and shared fixture readers, without changing frozen fixtures to hide a regression.
- Modify docs only for actual commands/results: `docs/TASK9_Customization.md`, `docs/task9-web-calibration.md`, `docs/task9-verification.md`, README.md.
- Deliver staged artifacts and reports under `/home/raya/.hermes/artifacts/`, not a new live service/root replacement.

**Interfaces:**
- Integration test data is explicitly TEST and originates from the actual C++ serializer/probe. Feed it to the real authenticated Go handler/store with an isolated database; read actual handler output through TS API/decoder/view-controller tests. No fake device response may be labeled physical evidence.
- Production-flow test consumes the same frozen wire/HTTP responses and calls actual DeviceBleSession/WifiSettingsController/DebugViewController code. No direct DOM source assertions as a substitute for behavior.

- [ ] Step 1: Build a failing integration test that changes one identity/hash/sequence in actual producer output and confirms rejection/rollback, then passes the unmodified actual output through ingest/query/TS decoding. Exercise shared-device identity mapping, expired operator access, boot change and retention gaps.

```go
func TestActualProducerPayloadCrossesAuthenticatedBoundary(t *testing.T) {
    raw, err := os.ReadFile("../../contracts/debug/v1/fixtures/http.json")
    if err != nil { t.Fatal(err) }
    upload, err := DecodeUpload(raw)
    if err != nil { t.Fatal(err) }
    if upload.SchemaVersion != 1 { t.Fatal("wrong fixture schema") }
}
```

Expand this seed test in the same task to call the real handler with disposable auth, query the committed row, and compare the ACK hash/body; merely loading JSON is not the integration deliverable. The Python probe check must independently verify the committed bytes still match actual C++ output.

- [ ] Step 2: Run the focused cross-language integration checks RED/GREEN separately. Then run `go test ./...`, `go test -race ./...`, and `go vet ./...` as three separate calls once Go is available; never claim they ran otherwise.
- [ ] Step 3: Run `npm test -- --run`, `npm run typecheck`, `npm run export:web` separately. Run `/home/raya/.platformio/penv/bin/python -m unittest discover -s tests -p 'test_*.py' -v` after confirming discoverable cases cover the repository's relevant host suites; report actual cases/skips, not historical pytest totals.
- [ ] Step 4: Run the full native suite with `/home/raya/.platformio/penv/bin/pio test -d firmware -e native`. Then issue every command below separately and capture its exit code. Preserve diagnostic wrappers and effective no-passkey MAX_BONDS/BLE_STORE_CONFIG_PERSIST settings. No uploads.

```text
/home/raya/.platformio/penv/bin/pio run -d firmware -e sigap_n16r8
/home/raya/.platformio/penv/bin/pio run -d firmware -e sigap_n16r8_sd
/home/raya/.platformio/penv/bin/pio run -d firmware -e sigap_n16r8_https_compile
/home/raya/.platformio/penv/bin/pio run -d firmware -e sigap_n16r8_sd_https_compile
/home/raya/.platformio/penv/bin/pio run -d firmware -e sigap_n16r8_debug
/home/raya/.platformio/penv/bin/pio run -d firmware -e sigap_ble_diag
/home/raya/.platformio/penv/bin/pio run -d firmware -e sigap_ble_wifi_test
```
- [ ] Step 5: Use an isolated local website/backend test instance and default viewport 1920x1080 / 390x844 for browser render/lifecycle smoke. Serving/starting the instance needs the permitted local-test scope; do not replace an existing helper/server or open public listeners. Exercise loading/empty/error/unsupported/TEST-log views, cancellation and responsive controls. Browser selection and physical BLE remain HUMAN_ACTION_REQUIRED. Credentials are not typed/read through ordinary browser tools; leave any actual operator login to the operator/vault workflow.
- [ ] Step 6: Stage source/contract hashes, BIN/ELF/map/web artifacts, test logs and exact exit codes. Compare served bytes only if a local test server was actually started; distinguish source/exported/served/already-open-tab state. Build checks do not prove radio, NVS preservation, remote HTTPS or timing. Deliver a startup side-effect audit covering constructors/framework/config/NimBLE/Wi-Fi, pins, filesystem/media and automatic NVS activity before asking to flash.

### Task 10: Independent review and permission-scoped handoff

**Files:**
- Create: an evidence manifest/report beside staged artifacts; update `docs/task9-verification.md` with exact software and outstanding physical gates.
- Read: approved spec, this plan, actual diffs, test outputs, codec fixtures and linked image evidence. Preserve HEAD unless separate commit permission is granted.

**Interfaces:**
- Review report classifies each acceptance criterion IMPLEMENTED / HOST_TESTED / LINKED / NOT_RUN / HUMAN_ACTION_REQUIRED / HOLD. No historical helper result is upgraded into production acceptance.
- Return findings with source lines, practical impact, cause and a concrete correction. The implementation owner fixes findings with their own RED/GREEN tests; main reviews evidence, not only a worker's completion claim.

- [ ] Step 1: Dispatch the fresh independent review only after verifying the required route and supervision. Review RF1-RF5, every emit/privacy insertion, exact API auth/cursor behavior, transport teardown, VFS exclusions and calibration/acquisition invariants. Native child results need independent artifact/target verification.
- [ ] Step 2: For each finding, add a focused regression before correction and rerun the affected task checks. If provider quota/toolchain/permission blocks the real check, label it blocked/NOT_RUN, preserve work and do not invent output or switch model.
- [ ] Step 3: Produce the final four-part report: problem, practical effect, evidence-backed cause, remedy with implementation/test/deployment boundaries. Include baseline/final HEAD, exact touched files and test counts, unresolved failures, artifact paths/hashes, no-commit/no-flash status and overall Task 9 HOLD.
- [ ] Step 4: Prepare, but do not execute, a precise physical permission request identifying the chosen full-app image, startup sensor/actuator/persistence/media side effects, flash/readback scope, observation window and stop rules. Existing diagnostic firmware and accepted helper remain active until separately approved replacement.
- [ ] Step 5: After future physical permission, assign an exclusive bounded serial-monitor subagent with verified PID/port/readiness/deadline before asking for one operator reproduction. USB/BLE/HTTPS acceptance, saved credential persistence, calibration mutations, SD and endurance remain distinct gates. Stop on denial/fault without automatic reset/flash/retry or widened permissions.

## Spec coverage matrix

| Spec section | Owning tasks and concrete evidence |
| --- | --- |
| 1 intent/scope/permissions | Global constraints, Tasks 5, 9, 10; actual production callers and separate physical gates |
| 2 existing risks | Tasks 3, 5, 6, 7; ordinal dispatch, idle GATT closure, shared session, one-shot replacement, VFS regression tests |
| 3 architecture/sinks | Tasks 1, 2, 4, 6, 8; one contract, independent cursors and source-labeled UI |
| 4 fields/privacy/bounds | Tasks 1, 2, 6, 7; fixed structs, strict fixtures, independent sequence, loss/backpressure, no payload emitters |
| 5 module coverage | Task 7 module matrix plus Task 9 actual composition probe |
| 6 USB/BLE | Tasks 1-3 and 5; 384/512/20 byte boundaries, queue ownership, explicit level, no retries/lease renewal |
| 7 HTTPS/backend | Tasks 1, 4, 6, 8, 9; exact ACK/hash, bounded storage, shared auth/quota, true cleanup |
| 8 website | Tasks 3, 5, 8, 9; retry allowlist, lifecycle/password, one owner, explicit sources, cancellable polling |
| 9 config | Tasks 2, 6, 7, 9; typed policy, volatile level, real full-app debug build and docs |
| 10 acceptance | Tasks 9, 10; full checks, actual producer integration, maps/startup audit, permission-scoped HIL |
| 11 execution | Preserved delegation method, fresh route preflight, scoped parallelism, supervision and owner review gate |

## Self-review and owner handoff

- All new APIs in this plan are proposed outputs with an owning task, not assertions about existing symbols.
- Read the whole approved spec and verify RF1-RF5 are pinned by the named owning tests. Check exact type/property names across tasks and reject silent alternate schemas.
- Check existing paths, proposed file collisions, line budgets, command runner flags, document placeholders and final Git state. Do not run an implementation command merely to make the plan look verified.
- The owner selected delegated execution and approved this written plan on 2026-10-22 WIB. Execute the authorized source/host tasks without repeated continuation questions; toolchain installation, flash, operational database changes, server replacement and physical testing still need their own applicable permissions.
