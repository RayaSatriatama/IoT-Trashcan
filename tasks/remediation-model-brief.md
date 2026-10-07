# Task 5 remediation: target model persistence boundary

Implement a bounded target adapter for FIS/Safety model profiles using the
existing pointer-last persistence concepts without duplicating acquisition
ProfileManager. Native fault injection remains required.

Requirements:

- two bounded slots per model key;
- readback, deterministic decode, hash and schema validation;
- committed active/previous metadata with pointer-last activation;
- staged candidate must never become active after activation failure;
- corruption falls back only to a committed previous generation;
- profile kind and target remain independent keys;
- public lifecycle behavior remains draft, validate, preview, commit, cancel;
- import finish only stages and does not activate;
- add regression tests for malformed, interrupted, corrupt, reload, rollback,
  and unchanged active identity on failed operations.

Ownership: `firmware/lib/models/**` and model-focused native tests. Do not edit
maintenance router/service or processing headers. If a target maintenance
adapter needs a narrow public interface, add it in the models boundary and
report the integration seam for the controller.
