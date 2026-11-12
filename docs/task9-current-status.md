# Current sensor integration checkpoint

This checkpoint publishes software progress, not full physical acceptance. Earlier dated verification documents remain historical and must not be read as a claim that all current sensor-to-web/FIS gates are complete.

## Owner-selected execution order

1. Verify all four active sensors in the full application.
2. Show their newly received data in home and device detail.
3. Verify real calibration/profile readiness and MCU fuzzy/safety output.
4. Then resolve the remaining ingest-stop cause.
5. Then complete controlled-stimulus and long-running durability/stability tests.

A prerequisite found to block an earlier item must be named; this ordering does not permit bypassing archive ACK semantics or presenting old data as fresh.

## Verified boundaries

- The reference-based standalone firmware produced 30 valid physical frames. HX711, ultrasound, SHT31 and MQ135 reported successful reads in all retained frames. No controlled movement, known-load calibration or gas accuracy was established.
- The exact saved full-application v2 image was restored after the reference test. Both authorized application writes passed readback; the restored application passed a bounded boot observation without panic. Those two writes consumed their explicit physical scope.
- The backend projects firmware raw/quality/provenance and MCU result records into public device, directory and location reads. Empty result sets are returned as an explicit `firmware_results: []`.
- The UI can render those real records and unavailable states. Host controls preserve zero/null, units, boot origin, time uncertainty and unconfigured model distinctions. Fixture data is not inserted into the operational database.
- A joined host control executed actual production reader/graph/factory/codec/batch-builder code. The actual Go contract validator accepted all 44 emitted requests across eight scenarios, including unconfigured/missing SHT31 profiles and failed reads. This is host evidence, not current physical streaming.
- A production-header echo probe demonstrated that an entire short echo can be missed between coarse polls. Actual target loop cadence remains unmeasured, so that counterexample is not accepted as the proven cause of the device's historical ultrasound timeout.

## Still incomplete

- Fresh full-application data from all four active sensors has not yet been demonstrated in the web. Recent projection observations showed an earlier boot's archived sensor records, not the standalone proof measurements.
- Ingest advanced through older-boot backlog before plateauing. The observed stop cause is unproven; a browser cache, credential failure or panic must not be asserted without matching evidence.
- The operational archive had not received MCU result records at the latest checkpoint. Production calibration/model readiness and actual FIS/safety output remain unverified. Do not install example coefficients or fabricate scores.
- Controlled before/stimulus/after responses, accuracy against references, and long-running ACK/journal/checkpoint/reclaim/restart acceptance remain open.

## Release and safety boundary

The owner separately authorized publication of the current source to main. Publication does not lift the consumed physical scope or grant additional serial connections, resets, flashes, erase/format, token/NVS/calibration commands or heater activation. A subsequent request for one read-only serial diagnostic session timed out without a response; no session was opened under it.

Local browser profiles, credentials, operational databases, generated executables, TLS preparation and raw session captures are not source-publication inputs and are excluded without deleting their local copies. A source merge is a development checkpoint, not approval to commission production calibration or declare the physical E2E complete.
