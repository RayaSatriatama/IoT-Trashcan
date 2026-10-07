# Task 5 remediation: runtime processing integration

Starting branch: `feat/firmware-processing-foundation`

Implement the application-owned processing loop without editing
`firmware/lib/processing/src/processing/processing_engine.h`.

Requirements:

- consume acquisition samples without making the logger handoff appear durable;
- retain bounded window/cycle state and make full handoff observable;
- capture immutable config/profile context before evaluation;
- use the existing shared boot sequence through `ResultRecordFactory`;
- expose an application-level sample-attempt to result-handoff flow;
- when models or samples are unavailable, preserve UNCONFIGURED/UNAVAILABLE
  semantics and never inject runtime readings;
- add native integration tests for active config, empty model state, sequence,
  provenance, and full result handoff behavior.

Ownership: `firmware/src/app/processing_runtime.h`, `firmware/src/app/sigap_app.*`,
new runtime integration tests only. Do not edit processing engine, model, fuzzy,
or safety implementation files.

Run focused tests and report exact output. Do not add physical-HIL assumptions.
