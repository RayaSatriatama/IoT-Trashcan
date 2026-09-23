# Device Sensor Research Design

## Purpose

Refine `/devices/:id` into an operational inspection view that keeps device context, compares the three waste streams, exposes the four-input collection decision, and presents physical sensor evidence per slot. The design must remain truthful for legacy records and simulation records.

## Product hierarchy

The page follows `Identify -> Compare -> Inspect`:

1. Device identity and aggregate decision summary.
2. Three comparable slot cards.
3. Per-slot, per-sensor research evidence and compact Fuzzy trace.

The existing SIGAP visual language remains unchanged: light surfaces, navy text, green identity, semantic stream accents, compact borders, and restrained motion.

## Data truth constraints

- Collection FIS inputs are exactly Fill, Weight, OGI, and Accumulation Time.
- Sensor health is not a FIS input.
- MQ-7, MQ-8, and DS18B20 are B3 safety and research channels, never collection FIS inputs.
- MQ values use raw or normalized/index units. `ppm` is accepted only when an engineering value and calibration version are explicitly supplied.
- Derived OGI and Accumulation Time are not physical sensors.
- Legacy records remain readable. Missing sensor evidence is shown as unavailable, never reconstructed.
- Invalid required collection inputs bypass FIS and result in Data Invalid.
- B3 Safety Alert takes precedence over an ordinary collection decision.
- Production thresholds and calibration formulas are not invented. Multi-input profiles and safety limits introduced here are simulation-only fixtures.

## Contract

### Ingest additions

Each slot may include:

- `ogi`
- `accumulation_hours`
- `sensors[]`

Each sensor item carries:

- `sensor_id`
- `sensor_type`
- `parameter`
- `raw_value`
- `engineering_value`
- `normalized_value`
- `unit`
- `sensor_state`
- `calibration_version`
- `recorded_at`

Allowed sensor states are `VALID`, `WARMING_UP`, `STALE`, `INVALID`, and `DISABLED`.

### Persisted decision evidence

Decision rows store additive nullable JSON fields:

- `trace_json`: four FIS inputs, memberships, triggered rule summary, score, status, version, and dominant reason.
- `safety_json`: B3 safety channels, state, and reasons.

Existing columns and payloads remain valid.

### Read API additions

The research response retains its current fields and adds:

- `decision_summary`
- per-slot `sensors`
- per-slot `derived_parameters`
- per-slot `fis_trace`
- per-slot `safety`

The summary selects the effective highest-severity slot, then score as a tiebreaker. Its priority score is the maximum available slot score. The mode is only reported as multi-parameter when the stored trace proves the four required inputs were evaluated.

## Simulation source

Simulation emits the documented sensor inventory:

- Organic: Fill Sensor, HX711, MQ-135, MQ-4.
- Inorganic: Fill Sensor, HX711, MQ-135.
- B3: Fill Sensor, HX711, MQ-135, MQ-7, MQ-8, DS18B20.

Simulation MQ channels use raw/index values and no `ppm` label. Simulation-only FIS profiles use the same four conceptual inputs with per-stream versions. Simulation-only safety configuration is isolated from production configuration.

## Interface design

### Header summary

The existing device hero gains three compact facts:

- Prioritas angkut: score out of 100.
- Slot paling kritis: stream and short reason.
- Mode keputusan: Fuzzy multi-parameter only when proven by trace data.

### Slot cards

All slot cards keep a shared order:

1. Stream and effective status.
2. Fill gauge.
3. Collection decision and score.
4. Weight utilization.
5. Weight, OGI, accumulation time, and update.
6. Compact memberships and related sensor list.

B3 additionally contains a visually separate safety panel for temperature, CO index, H2 index, and safety state. Stream red remains a category accent; alert severity uses the status treatment.

### Research panel

Research evidence is visible without a generic open/close gate. Desktop uses one group per slot with a semantic sensor table. Mobile converts each table row into a sensor card without horizontal scrolling.

Physical sensor rows show actual, raw, normalized, state, FIS membership where applicable, and role. Derived OGI and Accumulation Time appear in a separate derived section. The Fuzzy trace shows the four inputs, normalized values, memberships, rule count when available, score, status, reason, and version.

## Responsive behavior

- At 1024 pixels and above: three slot cards; three research groups when space permits.
- Tablet: two plus one slot grid; research groups wrap.
- Mobile: one slot card and one research group per row; sensor records become cards.
- Typography is preserved while layout reorganizes.
- All final content remains reachable and no horizontal overflow is permitted.

## Motion

Use the existing motion tokens and reduced-motion preference. Animate real score, gauge, and metric updates with bounded count-up/progress transitions. Do not add decorative continuous animation or animate raw sensor tables on every render.

## Verification

Automated checks cover validation, legacy compatibility, FIS memberships, trace persistence, safety precedence, API projection, simulator inventory, and frontend presentation helpers. Runtime verification uses simulation mode and all required viewports. Browser evidence must demonstrate desktop and mobile Device Detail, sensor groups, B3 safety separation, no overflow, and valid network responses.
