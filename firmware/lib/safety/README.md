# Safety B3

The safety evaluator is separate from the collection FIS. It consumes only a
validated, versioned Safety B3 model and bounded hazard inputs. There is no
factory production threshold.

Model parameters such as per-channel alert/clear thresholds and the required
number of consecutive clear cycles belong to `SafetyModelProfile`. They become
active through the model-profile validation and atomic activation workflow.
They are not `AppConfig` fields and do not hot-reload an evaluation already in
progress.

An invalid hazard input produces `UNAVAILABLE` or `FAULT`, never `CLEAR`.
Once an alert is latched it is released only after every required hazard input
meets the profile's clear threshold for the configured number of consecutive
cycles. A collection FIS score is deliberately absent from the evaluator API,
so a low collection score cannot release a safety alert.

Tests use explicitly named `TEST_ONLY` thresholds. Hardware-validated
production thresholds remain unavailable until approved scientific evidence
exists.
