import { describe, expect, it } from 'vitest';

import { createCalibrationApi, type SensorDiagnostic } from './calibration';

function jsonResponse(body: unknown, status = 200): Response {
  return new Response(JSON.stringify(body), { status, headers: { 'content-type': 'application/json' } });
}

describe('operator calibration API client', () => {
  it('sends an in-memory bearer token and typed command without persistence', async () => {
    let requestUrl = '';
    let requestInit: RequestInit | undefined;
    const diagnostic: SensorDiagnostic = {
      sensor_id: 'sht31_44', stream: 'organic', quality: 'UNCALIBRATED', reason: 'PROFILE_MISSING', raw: { present: true },
      converted_value: null, converted_unit: null, fresh: true, age_ms: 4, last_sequence: '7', profile_present: false,
      profile_version: null, profile_hash: null, calibration_state: 'IDLE', accepted_samples: 0, rejected_samples: 0,
    };
    const client = createCalibrationApi({ bearerToken: 'test-only-token' }, async (input, init) => {
      requestUrl = String(input);
      requestInit = init;
      return jsonResponse({ diagnostic });
    });

    await expect(client.getDiagnostics('SIGAP 01', 'sht31_44')).resolves.toMatchObject({ diagnostic });
    expect(requestUrl).toBe('/api/devices/SIGAP%2001/calibration/diagnostics?sensor_id=sht31_44');
    expect(new Headers(requestInit?.headers).get('authorization')).toBe('Bearer test-only-token');
    expect(requestInit?.body).toBeUndefined();
  });

  it('uses device-bound command body and surfaces an authorization failure', async () => {
    let payload = '';
    const client = createCalibrationApi({ bearerToken: 'operator-session' }, async (_input, init) => {
      payload = String(init?.body ?? '');
      return jsonResponse({ command: { command_id: 'c1', state: 'CAPTURING' } });
    });
    await expect(client.submit({ device_id: 'SIGAP-01', sensor_id: 'hx711_organic', operation: 'START', parameters: { reference_mass_kg: 2 } })).resolves.toMatchObject({ command: { command_id: 'c1' } });
    expect(JSON.parse(payload)).toMatchObject({ device_id: 'SIGAP-01', sensor_id: 'hx711_organic', operation: 'START' });
  });

  it('requires a non-empty operator token', () => {
    expect(() => createCalibrationApi({ bearerToken: ' ' })).toThrow('Operator authorization is required');
  });
});

