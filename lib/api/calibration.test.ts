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
      converted_value: null, converted_unit: null, fresh: true, age_ms: 4, ordering_unknown: false, future_clock_skew: false, last_sequence: '7', profile_present: false,
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
    let requestInit: RequestInit | undefined;
    const client = createCalibrationApi({ bearerToken: 'operator-session', csrfToken: 'csrf-in-memory' }, async (input, init) => {
      requestUrl = String(input);
      payload = String(init?.body ?? '');
      requestInit = init;
      return jsonResponse({ command: { command_id: 'c1', status: 'CAPTURING', state: 'CAPTURING' } });
    });
    await expect(client.submit({ device_id: 'SIGAP-01', sensor_id: 'hx711_organic', operation: 'START', parameters: { reference_mass_kg: 2 } })).resolves.toMatchObject({ command: { command_id: 'c1' } });
    expect(requestUrl).toBe('/api/device-control/commands/SIGAP-01');
    expect(JSON.parse(payload)).toMatchObject({ sensor_id: 'hx711_organic', operation: 'start', payload: { parameters: { reference_mass_kg: 2 } } });
    expect(new Headers(requestInit?.headers).get('x-sigap-csrf')).toBe('csrf-in-memory');
  });

  it('passes normalized MQ135 criteria through the existing HTTPS command envelope', async () => {
    let requestUrl = '';
    let payload = '';
    const criteria = {
      response_mode: 'normalized', samples: 3, min_duration_ms: 1000, max_age_ms: 250, max_variation_milli_percent: 5000,
      circuit_mv: 5000, divider_top_ohm: 10000, divider_bottom_ohm: 15000, topology: 1,
      hardware_confirmed: 1, reference_confirmed: 1, hardware_identity: 'SYNTHETIC-HW', reference_environment: 'LAB-01',
      reference_kind: 'operational', preparation_confirmed: 0, preparation_method: 'unknown', heating_duration_us: 0,
    } as const;
    const client = createCalibrationApi({ bearerToken: 'operator-session', csrfToken: 'csrf-in-memory' }, async (input, init) => {
      requestUrl = String(input);
      payload = String(init?.body ?? '');
      return jsonResponse({ command: { command_id: 'c-mq135', status: 'CAPTURING', state: 'CAPTURING' } });
    });
    await expect(client.submit({ device_id: 'SIGAP-01', sensor_id: 'mq135_gas', operation: 'SET_CRITERIA', parameters: criteria }))
      .resolves.toMatchObject({ command: { command_id: 'c-mq135' } });
    expect(requestUrl).toBe('/api/device-control/commands/SIGAP-01');
    expect(JSON.parse(payload)).toMatchObject({ sensor_id: 'mq135_gas', operation: 'set_criteria', payload: { parameters: criteria } });
    expect(payload).not.toContain('module_load_ohm');
  });

  it('requires a non-empty operator token', () => {
    expect(() => createCalibrationApi({ bearerToken: ' ' })).toThrow('Operator authorization is required');
  });

  it('normalizes direct command responses from the production handler and preserves result preview', async () => {
    const client = createCalibrationApi({ bearerToken: 'operator-session', csrfToken: 'csrf-in-memory' }, async () => jsonResponse({
      command_id: 'c-preview', device_id: 'SIGAP-01', sensor_id: 'hx711_organic', operation: 'preview', status: 'PREVIEW_READY',
      result: { proposal_hash: 'sha256:proposal', profile_version: '2', evidence_count: 3 },
    }));
    await expect(client.submit({ device_id: 'SIGAP-01', sensor_id: 'hx711_organic', operation: 'PREVIEW' })).resolves.toMatchObject({ command: { command_id: 'c-preview', status: 'PREVIEW_READY' } });
    await expect(client.getCommand('SIGAP-01', 'c-preview')).resolves.toMatchObject({ command: { command_id: 'c-preview' } });
  });

  it('rejects mutation before sending it when the in-memory CSRF value is absent', async () => {
    let calls = 0;
    const client = createCalibrationApi({ bearerToken: 'operator-session' }, async () => {
      calls += 1;
      return jsonResponse({});
    });
    await expect(client.submit({ device_id: 'SIGAP-01', sensor_id: 'mq4', operation: 'START' }))
      .rejects.toThrow('CSRF operator session is required');
    expect(calls).toBe(0);
  });

  it('sends the CSRF header when cancelling a queued command', async () => {
    let requestUrl = '';
    let requestInit: RequestInit | undefined;
    const client = createCalibrationApi({ bearerToken: 'operator-session', csrfToken: 'csrf-in-memory' }, async (input, init) => {
      requestUrl = String(input);
      requestInit = init;
      return new Response(null, { status: 204 });
    });
    await expect(client.cancelCommand('SIGAP-01', 'queued-command')).resolves.toBeUndefined();
    expect(requestUrl).toBe('/api/device-control/commands/SIGAP-01/queued-command');
    expect(requestInit?.method).toBe('DELETE');
    expect(new Headers(requestInit?.headers).get('x-sigap-csrf')).toBe('csrf-in-memory');
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
