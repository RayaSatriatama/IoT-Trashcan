# Task 9 plan: full integration and HIL validation

## Scope and gates

Task 9 starts only from the actual Task 8 merge commit. This plan does not
authorize a merge, firmware upload, reboot, SD mutation, destructive recovery,
or sensor commissioning. Every physical result is recorded separately from
native simulation and backend tests.

The Task 9 branch owns integration validation only. Network workers, durable
archive, BLE provisioning, and backend archive implementations remain in their
existing modules unless a reproducible integration defect requires a scoped
fix.

## Requirement traceability

| Requirement | Implementation seam | Test/evidence | Gate |
| --- | --- | --- | --- |
| Acquisition to archive preserves every attempt | `firmware/src/app`, storage worker and archive producer | Native producer-to-store identity reconciliation; target timing evidence | BLOCKED until fresh Task 9 run |
| Processing result keeps source/profile/config provenance | processing snapshot and telemetry-v3 builder | Native result envelope/schema/hash tests | NOT RUN |
| LittleFS fallback and SD_MMC 1-bit archive | storage media policy and migration worker | Fault-injected native recovery; attached SD test only with authorization | PHYSICAL BLOCKED |
| Pending batch is immutable until validated ACK | batch store, ACK journal, HTTPS boundary | Exact replay, malformed/conflict ACK, restart/reclaim tests | NOT RUN |
| WiFi reconnect is nonblocking | WiFi manager and network worker | Fake clock/driver timeout, backoff, acquisition-progress test | NOT RUN |
| Always-on BLE provisioning | provisioning service and browser client | Framing/session/authorization tests; browser secure-context test | PHYSICAL BLOCKED |
| Backend ingest is durable and idempotent | `internal/http`, `internal/telemetryarchive` | Staging HTTP round trip with exact frozen bytes | STAGING CREDENTIAL REQUIRED |
| Current projection does not regress on backlog | archive query/projection | Late-backfill and per-sensor integration tests | Task 8 evidence applicable; rerun in Task 9 |
| Resource and timing budgets are met | target policy, worker queues, stack probes | Target map, stack high-water, heap trend and scheduler latency | HARDWARE/IMAGE AUTHORIZATION REQUIRED |
| 24 h and 72 h endurance/capacity | SD archive and network delivery | Timestamped physical soak and reconciliation | BLOCKED: no endurance authorization |

## Work packages

1. **Firmware/storage integration:** verify acquisition, processing, archive,
   fallback, ACK, recovery, and no-reclaim behavior with native fault doubles.
2. **WiFi/BLE integration:** test state machines, bounded framing, credential
   privacy, scan/connection retries, and concurrent storage progress.
3. **Backend integration:** use a non-production endpoint and test identity to
   verify durable ACK, dedupe, conflict, authentication, export, and current
   projection. Never use production secrets.
4. **HIL/resource:** only after explicit authorization, perform non-destructive
   board checks, SD throughput/capacity, radio coexistence, and stack/heap
   measurements. Sensors, heaters, actuators, and safety evidence remain
   unavailable until physically connected.

## Verification sequence

Run focused tests within each work package, then the integrated suites:

```text
go test ./...
go test -race ./...
go vet ./...
uv tool run --from platformio pio test -e native
uv tool run --from platformio pio check -e native --fail-on-defect=low
uv tool run --from platformio pio run -e sigap_n16r8
uv tool run --from platformio pio run -e sigap_n16r8_sd
uv tool run --from platformio pio run -e sigap_n16r8_sd_spi
uv tool run --from platformio pio run -e sigap_bench
git diff --check
```

Schema verification must compile all 14 schemas, accept all 24 valid fixtures,
reject all 36 invalid fixtures, and preserve the frozen record/body hashes.
No fixture result is evidence of a physical sensor measurement.

## Exit criteria

Task 9 is not complete while any required software test is failing, the
staging HTTP boundary is unverified, or mandatory sensor/radio/SD endurance
evidence is unavailable. A blocked physical gate is reported explicitly and
is not converted to PASS by a mock or a successful build.
