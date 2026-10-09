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
    expect(requestUrl).toBe('/api/device-control/commands/SIGAP%2001/diagnostics?sensor_id=sht31_44');
    expect(new Headers(requestInit?.headers).get('authorization')).toBe('Bearer test-only-token');
    expect(requestInit?.body).toBeUndefined();
  });

  it('uses device-bound command body and surfaces an authorization failure', async () => {
    let requestUrl = '';
    let payload = '';
    const client = createCalibrationApi({ bearerToken: 'operator-session' }, async (input, init) => {
      requestUrl = String(input);
      payload = String(init?.body ?? '');
      return jsonResponse({ command: { command_id: 'c1', status: 'CAPTURING', state: 'CAPTURING' } });
    });
    await expect(client.submit({ device_id: 'SIGAP-01', sensor_id: 'hx711_organic', operation: 'START', parameters: { reference_mass_kg: 2 } })).resolves.toMatchObject({ command: { command_id: 'c1' } });
    expect(requestUrl).toBe('/api/device-control/commands/SIGAP-01');
    expect(JSON.parse(payload)).toMatchObject({ sensor_id: 'hx711_organic', operation: 'start', payload: { parameters: { reference_mass_kg: 2 } } });
  });

  it('requires a non-empty operator token', () => {
    expect(() => createCalibrationApi({ bearerToken: ' ' })).toThrow('Operator authorization is required');
  });

  it('normalizes direct command responses from the production handler and preserves result preview', async () => {
    const client = createCalibrationApi({ bearerToken: 'operator-session' }, async () => jsonResponse({
      command_id: 'c-preview', device_id: 'SIGAP-01', sensor_id: 'hx711_organic', operation: 'preview', status: 'PREVIEW_READY',
      result: { proposal_hash: 'sha256:proposal', profile_version: '2', evidence_count: 3 },
    }));
    await expect(client.submit({ device_id: 'SIGAP-01', sensor_id: 'hx711_organic', operation: 'PREVIEW' })).resolves.toMatchObject({ command: { command_id: 'c-preview', status: 'PREVIEW_READY' } });
    await expect(client.getCommand('SIGAP-01', 'c-preview')).resolves.toMatchObject({ command: { command_id: 'c-preview' } });
  });

  it('requests sensor-specific capabilities through the authenticated route', async () => {
    let url = '';
    const client = createCalibrationApi({ bearerToken: 'operator-session' }, async (input) => {
      url = String(input);
      return jsonResponse({ capabilities: { sensor_id: 'ds18b20_b3', references: [{ name: 'reference_temperature_c', unit: 'degC', required: true }], criteria: [], engineering_parameters: [] } });
    });
    await expect(client.getCapabilities('SIGAP-01', 'ds18b20_b3')).resolves.toMatchObject({ capabilities: { sensor_id: 'ds18b20_b3' } });
    expect(url).toBe('/api/device-control/commands/SIGAP-01/capabilities?sensor_id=ds18b20_b3');
  });
});
