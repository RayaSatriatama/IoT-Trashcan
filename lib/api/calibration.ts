import { ApiError, type Fetcher } from './client';

/** The website never persists this token. Keep the value in the caller's
 * authenticated session and pass it to this short-lived client instance. */
export type OperatorAuthorization = {
  bearerToken: string;
  /** Anti-forgery value from the short-lived operator session. Never persist it. */
  csrfToken?: string;
  operatorId?: string;
};

export const CALIBRATION_SENSOR_IDS = [
  'hx711_organic', 'hx711_inorganic', 'hx711_b3',
  'ultrasonic_organic', 'ultrasonic_inorganic', 'ultrasonic_b3',
  'mq4', 'mq135', 'mq7', 'mq8', 'ds18b20_b3', 'sht31_44', 'sht31_45',
] as const;
export type CalibrationSensorId = typeof CALIBRATION_SENSOR_IDS[number] | 'mq135_gas';
export type CalibrationTransport = 'ble' | 'https';
export type CalibrationState = 'IDLE' | 'QUEUED' | 'DELIVERED' | 'ACCEPTED' | 'CAPTURING' | 'VALIDATING' | 'VALIDATED' | 'PREVIEW_READY' | 'COMMITTING' | 'COMMITTED' | 'CANCELLED' | 'EXPIRED' | 'FAILED';
export type CalibrationQuality = 'READY' | 'WARMING_UP' | 'HEATER_PHASE' | 'CALIBRATING' | 'UNCALIBRATED' | 'STALE' | 'TIMEOUT' | 'DISCONNECTED' | 'CRC_ERROR' | 'OUT_OF_RANGE' | 'INVALID';

export type RawEvidence = Record<string, unknown> & {
  present: boolean;
  value?: number | null;
  unit?: string;
  identity_verified?: boolean;
  crc_valid?: boolean;
  temperature_crc_valid?: boolean;
  humidity_crc_valid?: boolean;
  conversion_complete?: boolean;
  heater_ready?: boolean;
};

export type SensorDiagnostic = {
  sensor_id: CalibrationSensorId;
  stream: 'organic' | 'inorganic' | 'b3' | string;
  quality: CalibrationQuality;
  reason: string;
  raw: RawEvidence | null;
  converted_value: number | Record<string, unknown> | null;
  converted_unit: string | null;
  fresh: boolean | null;
  age_ms: number | null;
  ordering_unknown: boolean;
  future_clock_skew: boolean;
  last_sequence: string | null;
  profile_present: boolean;
  profile_version: string | null;
  profile_hash: string | null;
  calibration_state: CalibrationState;
  accepted_samples: number;
  rejected_samples: number;
  boot_id?: string;
  sample_origin?: 'SIMULATED' | 'PHYSICAL' | 'UNKNOWN';
  origin_firmware_version?: string | null;
};

export type CalibrationCapabilities = {
  sensor_id: CalibrationSensorId;
  references: Array<{ name: string; unit: string; required: boolean; min?: number; max?: number }>;
  criteria: Array<{ name: string; unit: string; required: boolean; min?: number; max?: number }>;
  engineering_parameters: Array<{ name: string; unit: string; required: boolean }>;
};

export type CalibrationCommand = {
  command_id: string;
  device_id: string;
  sensor_id: CalibrationSensorId;
  operation: string;
  status: CalibrationState;
  state?: CalibrationState;
  expires_at: string;
  accepted_at?: string;
  error?: string;
  result?: Record<string, unknown> | null;
};

export type CalibrationPreview = {
  sensor_id: CalibrationSensorId;
  proposal_hash: string;
  profile_version: string;
  parameters: Record<string, number | string | boolean | null>;
  evidence_count: number;
  evidence_hash: string;
  expires_at: string;
};

export type CalibrationCommandRequest = {
  command_id?: string;
  device_id: string;
  sensor_id: CalibrationSensorId;
  operation: 'START' | 'SET_CRITERIA' | 'CAPTURE' | 'VALIDATE' | 'PREVIEW' | 'COMMIT' | 'CANCEL' | 'ROLLBACK' | 'EVIDENCE' | 'ACTIVE_PROFILE' | 'HISTORY';
  session_id?: string;
  payload?: Record<string, unknown>;
  parameters?: Record<string, number | string | boolean>;
  references?: Record<string, number>;
  criteria?: Record<string, number>;
  expected_profile_version?: string;
  expected_profile_hash?: string;
  expires_at?: string;
};

type CalibrationResponse = {
  diagnostic?: SensorDiagnostic;
  capabilities?: CalibrationCapabilities;
  command?: CalibrationCommand;
  preview?: CalibrationPreview;
  session_id?: string;
};

export type CalibrationCommandResponse = CalibrationResponse & { command: CalibrationCommand };

function normalizeCommandResponse(body: CalibrationResponse | CalibrationCommand): CalibrationCommandResponse {
  if (typeof body === 'object' && body !== null && 'command' in body && body.command) return body as CalibrationCommandResponse;
  if (typeof body === 'object' && body !== null && 'command_id' in body) return { command: body as CalibrationCommand };
  throw new Error('Respons command tidak valid');
}

export type CalibrationApi = ReturnType<typeof createCalibrationApi>;

function requestId(): string {
  const cryptoValue = (globalThis as unknown as { crypto?: { randomUUID?: () => string } }).crypto;
  if (cryptoValue?.randomUUID) return cryptoValue.randomUUID();
  return `web-${Date.now()}-${Math.floor(Math.random() * 1_000_000)}`;
}

export function createCalibrationApi(
  auth: OperatorAuthorization,
  fetcher: Fetcher = fetch,
  baseUrl = configuredBaseUrl(),
) {
  if (!auth.bearerToken.trim()) throw new Error('Operator authorization is required');

  const path = (value: string) => `${baseUrl.replace(/\/$/u, '')}${value}`;
  const headers = (mutating = false) => {
    if (mutating && !auth.csrfToken?.trim()) {
      throw new Error('CSRF operator session is required for calibration changes');
    }
    return {
      Accept: 'application/json',
      'Content-Type': 'application/json',
      Authorization: `Bearer ${auth.bearerToken}`,
      ...(mutating ? { 'X-SIGAP-CSRF': auth.csrfToken!.trim() } : {}),
    };
  };

  async function parse<T>(response: Response): Promise<T> {
    const text = await response.text();
    let body: unknown = null;
    try { body = text ? JSON.parse(text) : null; }
    catch { throw new ApiError('Server returned malformed JSON', response.status); }
    if (!response.ok) {
      const message = typeof body === 'object' && body !== null && 'error' in body && typeof body.error === 'string'
        ? body.error : response.statusText || `Request failed with status ${response.status}`;
      throw new ApiError(message, response.status);
    }
    return body as T;
  }

  async function get<T>(url: string, signal?: AbortSignal): Promise<T> {
    return parse<T>(await fetcher(path(url), { method: 'GET', headers: headers(), signal }));
  }
  async function post<T>(url: string, payload: unknown, signal?: AbortSignal): Promise<T> {
    return parse<T>(await fetcher(path(url), { method: 'POST', headers: headers(true), body: JSON.stringify(payload), signal }));
  }

  return {
    getDiagnostics: async (deviceId: string, sensorId: CalibrationSensorId, signal?: AbortSignal) => {
      const body = await get<CalibrationResponse | SensorDiagnostic>(
        `/api/device-control/commands/${encodeURIComponent(deviceId)}/diagnostics?sensor_id=${encodeURIComponent(sensorId)}`,
        signal,
      );
      if (typeof body === 'object' && body !== null && 'diagnostic' in body) {
        return body as CalibrationResponse;
      }
      return { diagnostic: body as SensorDiagnostic };
    },
    getCapabilities: (deviceId: string, sensorId: CalibrationSensorId, signal?: AbortSignal) =>
      get<CalibrationResponse>(`/api/device-control/commands/${encodeURIComponent(deviceId)}/capabilities?sensor_id=${encodeURIComponent(sensorId)}`, signal),
    submit: async (request: CalibrationCommandRequest, signal?: AbortSignal) => {
      const commandPayload = {
        ...(request.payload ?? {}),
        ...(request.parameters ? { parameters: request.parameters } : {}),
        ...(request.references ? { references: request.references } : {}),
        ...(request.criteria ? { criteria: request.criteria } : {}),
        ...(request.expected_profile_hash ? { expected_profile_hash: request.expected_profile_hash } : {}),
        ...(request.expected_profile_version ? { expected_profile_version: request.expected_profile_version } : {}),
      };
      const body = await post<CalibrationResponse | CalibrationCommand>(`/api/device-control/commands/${encodeURIComponent(request.device_id)}`, {
        command_id: request.command_id ?? requestId(),
        session_id: request.session_id ?? '',
        sensor_id: request.sensor_id,
        operation: request.operation.toLowerCase(),
        payload: commandPayload,
        expires_at: request.expires_at ?? new Date(Date.now() + 120_000).toISOString(),
      }, signal);
      return normalizeCommandResponse(body);
    },
    getCommand: async (deviceId: string, commandId: string, signal?: AbortSignal) =>
      normalizeCommandResponse(await get<CalibrationResponse | CalibrationCommand>(`/api/device-control/commands/${encodeURIComponent(deviceId)}/${encodeURIComponent(commandId)}`, signal)),
    cancelCommand: async (deviceId: string, commandId: string, signal?: AbortSignal) => {
      const response = await fetcher(path(`/api/device-control/commands/${encodeURIComponent(deviceId)}/${encodeURIComponent(commandId)}`), {
        method: 'DELETE', headers: headers(true), signal,
      });
      if (response.status === 204) return;
      await parse<void>(response);
    },
  };
}

function configuredBaseUrl(): string {
  if (typeof process !== 'undefined' && process.env.EXPO_PUBLIC_API_BASE_URL) {
    return process.env.EXPO_PUBLIC_API_BASE_URL.replace(/\/$/u, '');
  }
  return '';
}
