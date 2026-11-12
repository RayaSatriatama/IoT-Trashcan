import { describe, expect, it } from 'vitest';

import { diagnosticFromResearch } from './research';

describe('research diagnostics boundary', () => {
  it('treats a null sensor collection as unavailable instead of crashing', () => {
    expect(diagnosticFromResearch('hx711_organic', {
      device_id: 'task9-device',
      decision_summary: { priority_score: null, critical_stream: null, critical_reason: null, mode: null },
      slots: [{
        stream: 'organic',
        recorded_at: null,
        raw: { fill: null, weight: null, gas_raw: null, temperature: null },
        normalized: { fill_pct: null, weight_utilization: null, ogi: null, accumulation_hours: null, temperature: null },
        sensor_health: null,
        fis_score: null,
        fis_status: null,
        fis_version: null,
        dominant_reason: null,
        safety_state: null,
        sensors: null,
        derived_parameters: { ogi: null, accumulation_hours: null },
        fis_trace: null,
        safety: null,
      }],
    })).toBeNull();
  });
});
