---
name: dicoding-review-auditor
description: Audit or conduct an evidence-gated Dicoding submission review across course formats when a course-specific rubric and a consistent, extension-ready review packet are required.
---

# Dicoding Review Auditor

Use this skill with the active course-specific reviewer skill. The course skill remains the authority for rubric wording, official exceptions, scripts, templates, score rules, and submission-specific checks. This skill supplies the shared evidence workflow, packet contract, and final consistency audit.

Do not use this skill to replace a course rubric, alter a student submission, submit a decision to Dicoding, or treat automation as rubric authority.

## Select a mode

Choose the narrowest mode that matches the request.

- `FULL_REVIEW`: no completed, trusted packet exists. Perform every applicable phase in [the review contract](references/review-contract.md).
- `AUDIT_ONLY`: a prior review and its artifacts exist. Inspect them read-only, normalize evidence into a packet, and report whether the prior decision is audit-ready. Do not silently repair its decision or student-facing letter.
- `RESUME`: a packet exists with an explicit unresolved phase or `BLOCKED` condition. Preserve completed evidence, run only the missing or invalidated phase, then complete the shared final gates.

For all modes, first identify the submission root, review identifier, class-specific skill, and official rubric sources. Read the applicable class guidance before interpreting a finding.

## Lock authority and intake

Create or update the packet only after recording the ordered authority sources:

1. official rubric or submission guideline;
2. official common cases and exceptions;
3. class-specific reviewer skill;
4. feedback-style standard;
5. automation scripts and generated checklists.

Record immutable intake metadata for every raw artifact and automation run, including source, timestamp, concrete command, tool version, digest, and a link from the run to its raw evidence ID. Preserve raw output or an immutable reference to it. Derived findings must cite raw evidence by ID and must never overwrite, relabel, or summarize away the raw result. Lock the packet to the canonical profile SHA-256 digest. Each rubric reference must name both an authority ID and a precise section or locator.

Mark every authority source `ACTIVE` or explicitly `NOT_APPLICABLE`. A not-applicable authority category must cite a prior active official authority and its precise locator. Every skip rule must also cite an active official rubric, exception, or common-case source with a precise locator.

Use the stable structures in [class-profile.schema.json](references/class-profile.schema.json) and [review-packet.schema.json](references/review-packet.schema.json). Keep arrays, `id` values, and `order` stable so a browser extension or web worker can render any course without class-specific parsing.

Write the canonical normalized packet to `<submission-root>/review-packet.json`. Treat that exact location as the producer and consumer contract for repository tooling, browser extensions, and web workers.

## Full-review routing

Read [the review contract](references/review-contract.md) before a full review. The required order is:

1. lock profile and authorities;
2. preserve intake and raw automation;
3. compare Student Note with available submission history;
4. review mandatory criteria manually;
5. perform required runtime and browser checks;
6. review optional criteria and calculate a score only when the course profile allows it;
7. cross-check negative, ambiguous, anomalous, or conflicting findings against primary authority;
8. generate the student-facing document from the final packet and the official class template;
9. run the packet auditor and perform final manual sign-off.

Do not treat a source-code inspection, HTTP status, static screenshot, or autograder result as a substitute for a required interactive browser trace. If a reviewer capability or environment prevents a required check, record the condition and return `BLOCKED`; do not convert it into a student failure.

## Delegation and exclusive resources

For a full review, evaluate independent roles such as artifact and dataset inspection, notebook or source analysis, runtime or browser verification, and rubric cross-check. When multi-agent capability is available and a role is eligible, delegate it. Parent ownership is allowed only for a structured non-isolation constraint tied to the role's explicit capability or exclusive resource. Preference is not a constraint. Record the role, eligibility result, owner, scope, returned evidence, and any exception in the packet.

Children are read-only evidence collectors. They may inspect artifacts and report reproducible evidence, but cannot write the packet, checklist, notes, approval or rejection letter, final decision, or rating. The main reviewer validates child evidence and owns synthesis.

Assign one owner only to each browser target, local runtime, port, or other mutable shared resource. Do not run parallel browser owners against the same target. Record the owner and target before the action. A runtime result must bind to a compatible delegation-role capability and that owner's exclusive resource. A browser trace must repeat the same owner and target resource.

## Evidence and feedback boundaries

Assess every required submission artifact through `submission_artifacts`. Every resolved mandatory criterion needs the profile's authority reference, a reason, primary evidence, and a manual owner. `SKIPPED` and `NOT_APPLICABLE` need an applicable profile rule, authoritative-exception classification, and evidence. A negative, anomalous, failed-automation, or missing-required-artifact finding needs a cross-check that maps the evidence to the official requirement.

Optional findings are advisory until all prerequisite mandatory criteria are resolved and passed, unless the profile explicitly permits a non-scoring advisory review. For `optional_count`, select the declared tier from the count of met optional criteria and use its value. Keep every resolved scored optional criterion in the calculation reference set, including zero-point or `NOT_MET` results. Never infer a rating formula from another class.

Generate student-facing output only after the packet decision is final. Use the active class template and boilerplate. A final student artifact must be readable, match its recorded digest, and agree with the packet decision and rating. Include only student-relevant status, submission facts, evidence that explains a required revision, and actionable learning guidance. Exclude internal paths, reviewer logs, raw command output, private URLs, credentials, agent discussion, and placeholders.

## Final gate

Run the deterministic auditor after the final packet is assembled:

```bash
python3 scripts/audit_review.py --profile path/to/profile.json --packet path/to/review-packet.json
```

Interpret its exit code as follows:

- `0`: structurally and semantically ready for final manual sign-off;
- `1`: invalid or inconsistent packet. Correct the packet or perform the missing review phase;
- `2`: an explicit capability or environment block remains. Do not issue `LULUS` or `REVISI`.

Do not finish a one-prompt review until every requested applicable phase is either evidenced or explicitly `BLOCKED`. `READY`, `LULUS`, and `REVISI` require a final decision, fresh successful audit record bound to the canonical profile and packet hashes, and manual sign-off. Report a `BLOCKED` result with the relevant missing capability, attempted mitigation, retained evidence, and the precise next action required to resume. An unavailable capability that no active profile obligation or role needs does not block the review.
