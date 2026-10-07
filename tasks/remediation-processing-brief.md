# Task 5 remediation: processing semantics

Own the only edit surface for `firmware/lib/processing/src/processing/processing_engine.h`
and its processing tests. Do not edit app, models, fuzzy, or safety files.

Fix and test:

- latest-attempt readiness: READY followed by CALIBRATING, TIMEOUT, or
  DISCONNECTED must make the target unavailable for the current decision;
- distinguish freshness, latest-attempt state, and averaging;
- reject sample device/boot provenance mismatches against the cycle context;
- preserve monotonic timeline boundaries across boot IDs;
- validate FIS target/stream and Safety B3 target semantics;
- preserve units and quality/disposition when building model inputs;
- retain supported MQ ADC evidence without ppm/R0/OGI invention;
- profile refs with the same key but different version/hash are conflicts, not
  duplicates;
- model identity used by a cycle remains the captured snapshot after activation.

Write regression tests first, observe the expected failure, then implement and
run focused tests. Keep all containers bounded.
