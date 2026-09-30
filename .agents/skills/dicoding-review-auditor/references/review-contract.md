# Review Contract

This contract governs the shared process for `dicoding-review-auditor`. A class-specific skill supplies the course rubric, official exceptions, required artifacts, runtime obligations, score policy, and student-facing template. The global packet records those decisions in a stable, class-neutral form.

## Operating modes

| Mode | Starting state | Required outcome |
| --- | --- | --- |
| `FULL_REVIEW` | No trusted completed packet | Complete evidence-gated review and final packet |
| `AUDIT_ONLY` | Existing decision, artifacts, or feedback | Read-only assessment of audit readiness and inconsistencies |
| `RESUME` | Packet identifies unresolved or blocked work | Preserve valid evidence and complete only unresolved gates |

`AUDIT_ONLY` does not change the earlier decision, score, or student-facing material. It may identify that a historical decision cannot be independently confirmed. `RESUME` must identify the earlier packet and the exact phase being resumed.

## Authority and intake

Lock these sources in order before judging a criterion:

1. official rubric or submission guideline;
2. official common cases and exceptions;
3. active class-specific reviewer skill;
4. feedback-style standard;
5. automation scripts and generated checklists.

Each source declares `ACTIVE` or `NOT_APPLICABLE`. A not-applicable category is valid only when its declaration cites a prior active official rubric, exception, or common-case source by ID and precise locator. Every skip rule independently cites an active official rubric, exception, or common-case source by ID and precise locator.

Automation can discover a problem but cannot independently create a failure rule. A final negative result must identify an official requirement and primary evidence. A tool, network, credential, browser, runtime, or reviewer-environment failure is `BLOCKED` unless the primary evidence independently establishes a student defect.

Record profile identity and version, canonical profile SHA-256 digest, submission identity, source locations, authority references, and review mode. A criterion or cross-check authority reference includes the authority ID and a precise section or locator. Treat downloaded archives, extracted copies, autograder logs, browser recordings, screenshots, generated reports, and original history as intake artifacts. Raw evidence must have a stable ID, source, timestamp, immutable digest or immutable reference, and provenance. A derived finding must retain a `derived_from` relationship to raw IDs. Never modify raw output to make a derived conclusion appear cleaner.

## Packet fields and evidence

Use the schemas as the source for exact field names and enums:

- [Class profile schema](class-profile.schema.json) defines class policy, criteria, runtime obligations, scoring, and templates.
- [Review packet schema](review-packet.schema.json) defines the extension-facing review record.

Use stable `id` values and explicit `order` values. Do not encode a class-specific rubric layout as dynamic object keys. Keep evidence references explicit. Map every declared submission artifact type through `submission_artifacts`, including an explicit `MISSING` result when it was checked but absent. Record human-readable criterion identifiers in `official_id` and names in `title`.

Classify evidence as raw or derived. Examples of primary raw evidence include original submitted files, notebook cells, repository commits, test output, runtime output, browser traces, and official history. A source-code claim is evidence only for what it can establish. An interactive requirement needs a browser trace when the profile requires interaction.

A browser trace records all of these fields:

- URL and target identity;
- control or interaction target;
- before state;
- action;
- after state;
- conclusion tied to the criterion;
- owner, timestamp, and evidence reference.

An HTTP redirect, status code, DOM source, or static screenshot is not alone an interaction trace. Include them as supporting evidence when useful. A derived browser trace must cite raw browser, runtime, or screenshot evidence rather than a generic manifest or reviewer summary.

## Capability-gated delegation

For each independent role in a full review, assess:

1. whether a multi-agent capability is available;
2. whether the role can be isolated from another mutable resource or decision;
3. whether the role can perform the required read-only work;
4. whether delegation adds useful independent evidence.

Delegate every eligible role when multi-agent capability is available. Usual roles include artifact and dataset inspection, notebook or source review, runtime or browser verification, and rubric cross-check. Parent ownership requires a structured non-isolation reason that identifies either the role's explicit exclusive resource or its explicit capability. Reviewer preference is not a valid non-isolation reason.

Child role report:

```text
role_id:
scope:
eligibility: eligible | not_eligible
owner: child identifier | PARENT
status: DONE | BLOCKED_CAPABILITY
capability_gate_reason: required when owner is PARENT or status is BLOCKED_CAPABILITY
evidence: stable evidence IDs plus reproducible source, command, path, cell, line, URL, or browser action
limitations:
```

Children collect evidence read-only. They do not edit student artifacts, packet files, checklists, notes, letters, or decision documents. They do not approve, reject, assign ratings, or resolve conflicting evidence. The main reviewer validates reports and writes the packet.

Only one owner may run or control a particular local runtime, port, session, or browser target. Assign that owner before starting the resource. Other roles may inspect saved outputs after the owner releases or records the evidence. A shared target that cannot be exclusively owned is a capability constraint, not a reason to run duplicate sessions.

## Required phases

### 1. Profile lock and raw automation

Load the course profile and authorities, then preserve intake. Run class-provided automation after its concrete command and tool version are recorded. Save raw output and exit status, link the run to its matching raw evidence ID, and retain the digest even if the output looks inaccurate. A failed run, null exit status, or placeholder command requires a resolved cross-check or a blocked review policy before finalization. Create separate derived findings for interpretation.

### 2. Student context and history

Record Student Note content or its absence, available submission and review history, and a comparison result. Flag only evidence-based anomalies, such as a claimed change absent from the history or an earlier unresolved rejection point. A short or non-technical note alone is not an anomaly.

### 3. Manual mandatory review

Review each mandatory criterion against the locked authority. Record criterion category, official ID, status, reason, authority classification, rubric references, primary evidence IDs, manual owner, and order. If evidence conflicts, add a cross-check and revisit the narrowest primary source.

`SKIPPED` and `NOT_APPLICABLE` require an applicable profile rule, authoritative-exception classification, and evidence. `BLOCKED` requires a relevant capability or environment record plus attempted mitigation. A resolved `PASS`, `FAIL`, `MET`, or `NOT_MET` cannot rely on automation alone. A missing required submission artifact is a negative finding and needs a confirmed cross-check before it supports a decision.

### 4. Runtime and browser verification

Perform each active profile-required runtime check. An `ARTIFACT_PRESENT` trigger activates its obligation only when the named submission artifact is recorded as `PRESENT`. Keep commands, configuration relevant to the result, exit status, owner assignment, target resource, and output references. The runtime owner must match an active role assignment whose capability is compatible with the obligation, and the target must match that owner's exclusive resource. For browser obligations, use the single owner rule and repeat the runtime owner and target resource in the browser trace. Interact with the required control and retain a complete trace. If a deployment claim or URL triggers a deployment obligation, inspect the deployment according to the profile; a redirect alone does not prove failure or success.

### 5. Optional review and score

Assess optional criteria only as the profile permits. Calculate a score only after mandatory prerequisites pass and optional scoring is eligible. Use the profile's declared scoring mode and thresholds. For `optional_count`, map the number of met criteria to exactly one declared tier and use that tier's value. Include every resolved scored optional result in the calculation references, including zero-point and `NOT_MET` results. Do not award, subtract, or transfer points using a formula from another class. Record an advisory optional finding separately when the profile permits review but not scoring.

### 6. Cross-check and integrity

Create a cross-check for every negative, ambiguous, anomalous, or conflicting finding. Each cross-check names the criterion or issue, source authority, evidence IDs, conclusion, and owner. Verify provenance: raw results remain unchanged, derived claims cite raw evidence, and no external claim lacks its source.

### 7. Feedback and final consistency

Generate notes and approval or rejection material only after the decision packet is stable. Start from the active class template and its matching boilerplate. Keep one issue per feedback point. State the applicable criterion, observable submission fact, impact, and a clue or learning reference appropriate to the course.

Student-facing output may contain:

- decision and criterion status;
- score or rating when the class policy permits it;
- relevant facts from submitted artifacts or public deployment URLs;
- concise browser or runtime evidence when it explains the decision;
- actionable revision or learning guidance.

Student-facing output must not contain:

- absolute POSIX paths, local or internal reviewer paths, logs, raw commands, temporary files, or patched artifacts;
- agent roles, internal disagreement, autograder internals, or reviewer-only checklists;
- credentials, private URLs, access tokens, or unredacted secrets;
- unresolved template markers or generic boilerplate unrelated to the packet.

The final student artifact must resolve to a readable local file during audit. Its digest, decision label, template decision and audience, body status, rating field, and body rating must agree with the packet. Internal screenshots, manifests, and checklists do not use a feedback template. If any result differs, return to the narrowest invalid phase and regenerate the affected document from the packet.

## Completion and blocked behavior

The phase state is:

```text
INTAKE -> PROFILE_LOCKED -> RAW_AUTOMATION -> CONTEXT_REVIEW
  -> MANDATORY_REVIEW -> RUNTIME_REVIEW -> OPTIONAL_REVIEW
  -> RUBRIC_CROSSCHECK -> FEEDBACK_DRAFT -> CONSISTENCY_CHECK

CONSISTENCY_CHECK -> READY
CONSISTENCY_CHECK -> NEEDS_RERUN
ANY_REQUIRED_GATE -> BLOCKED
READY -> LULUS | REVISI
```

`READY`, `LULUS`, and `REVISI` require all applicable gates:

- locked profile and authority sources;
- immutable raw intake and automation records;
- Student Note and history comparison;
- complete mandatory criteria with authority, primary evidence, and manual ownership;
- required runtime and browser traces;
- eligible optional scoring when a score is issued;
- cross-checks for negative, ambiguous, anomalous, or conflicting findings;
- template-safe student-facing output generated from the final packet;
- matching decision, note, letter, and score;
- fresh auditor success recorded with the concrete command, current tool version, canonical profile hash, canonical packet-subject hash, result, and issue count;
- manual sign-off.

Run:

```bash
python3 scripts/audit_review.py --profile path/to/profile.json --packet path/to/review-packet.json
```

Exit `0` permits final manual sign-off. Exit `1` means the packet is invalid or inconsistent. Exit `2` means an explicit blocked condition remains. A review requested in one prompt must complete all applicable gates in that turn. If that cannot happen, return `BLOCKED` with the missing capability or external dependency, evidence already retained, mitigation attempted, and the exact resume action. Do not issue a final pass, rejection, or rating while the completion gate is blocked.
