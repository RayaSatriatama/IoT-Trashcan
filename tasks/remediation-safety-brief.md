# Task 5 remediation: persistent safety evaluator boundary

Own `firmware/lib/safety/**` and safety-focused tests only. Do not edit
`processing_engine.h`.

Remove or constrain the stateless evaluator path so a runtime cannot silently
reset B3 latch state per cycle. Keep the instance-owned evaluator API, model
change semantics, invalid input behavior, validated consecutive clear cycles,
and the rule that collection/FIS score cannot clear Safety.

Add direct regression coverage for alert, non-clear below-alert values, two
validated clear cycles, invalid input during a latch, and model identity change.

Run focused native tests and report exact results. The controller will wire the
persisted instance into ProcessingPipeline after this task.
