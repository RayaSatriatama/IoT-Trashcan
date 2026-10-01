# Implementation Plan: SIGAP Task 4 Maintenance and Calibration

## Baseline and scope

- Task 3 merge baseline: `main` at `706117c7d9ebc9e38d583faacf683f5c85a76f4b`.
- Branch: `feat/firmware-maintenance-calibration`.
- Scope: local bounded USB maintenance, AppConfig maintenance lifecycle, concrete SecretStore, versioned calibration profiles, calibration sessions, safe profile activation, redacted status, import/export, rollback, and native/target verification.
- Deferred: FIS/safety processing, logger/archive, telemetry transport, Wi-Fi worker, backend, migration 005, and Task 5.

## Architecture decisions

- Preserve Task 1 AppConfig A/B and apply-on-reboot semantics. Maintenance mutates only a draft and uses an explicit allowlist.
- Keep SecretStore outside AppConfig and profiles. Persist complete bounded credential blobs with integrity metadata; expose presence only to maintenance.
- Keep calibration profiles separate from AppConfig. Use deterministic typed envelopes, target-bound validation, active and previous generations, and pointer-last activation.
- Use immutable profile snapshots at acquisition-attempt boundaries. A sample keeps the profile identity used for that attempt.
- Keep USB parsing fixed-capacity and cooperative. Maintenance work is bounded per poll and cannot block acquisition.
- Keep calibration target-local. Trial raw evidence is distinct from operational READY data, and other channels continue running.

## Task list

### Phase 1: bounded local maintenance

- [x] Add parser/service transport seam, command grammar, bounded output, DeviceMode, and parser tests.
- [x] Add ConfigManager allowlisted field edits, reset/cancel/preview/status integration, and lifecycle tests.

### Phase 2: persistence boundaries

- [x] Implement concrete SecretStore blob persistence, redaction, secure clearing, and fault tests.
- [x] Implement deterministic profile envelope, A/B active/previous store, rollback/reset, and import/export tests.

### Checkpoint: persistence

- [ ] Secret values remain absent from status, exports, telemetry, and diagnostics.
- [ ] Failed writes/readback/pointer switches leave the previous valid profile authoritative.
- [ ] AppConfig commit remains reboot-required and does not hot reload sampling.

### Phase 3: calibration and runtime activation

- [x] Add target-specific bounded calibration session and online statistics.
- [x] Add immutable runtime profile provider/snapshot activation and SigapApp integration.
- [x] Add audit/provenance sink and redacted maintenance status.

### Checkpoint: integrated verification

- [ ] Calibration target is local, other acquisition channels continue, and trial data is not operational READY input.
- [ ] Profile commit activates only a complete validated snapshot; import requires draft/validate/preview/commit.
- [ ] Native tests, cppcheck, Go regression, and all target builds pass.

## Risks and mitigations

| Risk | Mitigation |
| --- | --- |
| USB parser blocks acquisition | Fixed line and byte budgets, cooperative polling, native fake transport tests. |
| Secret pair is partially persisted | Encode SSID/password as one integrity-checked blob and inject backend faults. |
| Profile activation invalidates in-use pointers | Copy immutable typed snapshots at attempt boundaries and retain profile identity in records. |
| Calibration invents scientific values | Require explicit typed criteria/reference evidence; leave ppm/R0/OGI unavailable without approved evidence. |
| NVS power loss creates partial active state | Write inactive generation, read back/decode/validate/hash-check, switch pointer last. |
| Existing plans are overwritten | Keep Task 1/Task 3 plan files unchanged; use Task 4-specific plan/todo files. |

## Traceability targets

- PRD v1.3 §6A, §7, §7A-§7D, §8; AC14, AC17, AC18, AC21, AC30-AC35.
- TSD v1.2 §§3A, 7, 11, 12, 13, 15.13 and the profile/maintenance verification matrix.
