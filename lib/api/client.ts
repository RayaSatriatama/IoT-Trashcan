import type {
  CanonicalStatus,
  DeviceDetail,
  DeviceSummaryResponse,
  HistoryFilters,
  HistoryPageRequest,
  HistoryResponse,
  LocationDetail,
  LocationListResponse,
  OverviewResponse,
  ResearchResponse,
  RuntimeResponse,
  SensorHistoryPage,
} from './types';

export type Fetcher = (input: RequestInfo | URL, init?: RequestInit) => Promise<Response>;

export class ApiError extends Error {
  readonly status: number;

  constructor(message: string, status: number) {
    super(message);
    this.name = 'ApiError';
    this.status = status;
  }
}

function configuredBaseUrl(): string {
  if (typeof process !== 'undefined' && process.env.EXPO_PUBLIC_API_BASE_URL) {
    return process.env.EXPO_PUBLIC_API_BASE_URL.replace(/\/$/, '');
  }
  return '';
}

function pathWithBase(path: string): string {
  return `${configuredBaseUrl()}${path}`;
}

async function parseResponse<T>(response: Response): Promise<T> {
  const text = await response.text();
  let body: unknown;
  try {
    body = text ? JSON.parse(text) : null;
  } catch {
    throw new ApiError('Server returned malformed JSON', response.status);
  }
  if (!response.ok) {
    const message = typeof body === 'object' && body !== null && 'error' in body && typeof body.error === 'string'
      ? body.error
      : response.statusText || `Request failed with status ${response.status}`;
    throw new ApiError(message, response.status);
  }
  return body as T;
}

function encodeId(id: string): string {
  return encodeURIComponent(id);
}

export function createApiClient(fetcher: Fetcher = fetch): {
  fetchOverview(signal?: AbortSignal): Promise<OverviewResponse>;
  fetchLocations(params?: { query?: string; status?: CanonicalStatus }, signal?: AbortSignal): Promise<LocationListResponse>;
  fetchLocation(id: string, signal?: AbortSignal): Promise<LocationDetail>;
  fetchDevice(id: string, signal?: AbortSignal): Promise<DeviceDetail>;
  fetchDevices(signal?: AbortSignal): Promise<DeviceSummaryResponse>;
  fetchResearch(id: string, signal?: AbortSignal): Promise<ResearchResponse>;
  fetchSensorHistory(id: string, sensorId: string, cursor?: string, signal?: AbortSignal, bootId?: string): Promise<SensorHistoryPage>;
  fetchRuntime(signal?: AbortSignal): Promise<RuntimeResponse>;
  fetchHistory(filters?: HistoryFilters, page?: HistoryPageRequest, signal?: AbortSignal): Promise<HistoryResponse>;
  buildExportURL(filters?: HistoryFilters): string;
} {
  async function get<T>(path: string, signal?: AbortSignal): Promise<T> {
    const response = await fetcher(pathWithBase(path), {
      method: 'GET',
      headers: { Accept: 'application/json' },
      signal,
    });
    return parseResponse<T>(response);
  }

  return {
    fetchOverview: (signal) => get<OverviewResponse>('/api/overview', signal),
    fetchLocations: (params = {}, signal) => {
      const query = new URLSearchParams();
      if (params.query?.trim()) query.set('q', params.query.trim());
      if (params.status) query.set('status', params.status);
      const suffix = query.toString() ? `?${query.toString()}` : '';
      return get<LocationListResponse>(`/api/locations${suffix}`, signal);
    },
    fetchLocation: (id, signal) => get<LocationDetail>(`/api/locations/${encodeId(id)}`, signal),
    fetchDevice: (id, signal) => get<DeviceDetail>(`/api/devices/${encodeId(id)}`, signal),
    fetchDevices: (signal) => get<DeviceSummaryResponse>('/api/devices', signal),
    fetchResearch: (id, signal) => get<ResearchResponse>(`/api/devices/${encodeId(id)}/research`, signal),
    fetchSensorHistory: (id, sensorId, cursor, signal, bootId) => { const query = new URLSearchParams({sensor_id:sensorId,limit:'200'}); if(cursor) query.set('cursor',cursor); if(bootId) query.set('boot_id',bootId); return get<SensorHistoryPage>(`/api/devices/${encodeId(id)}/sensor-history?${query}`, signal); },
    fetchRuntime: (signal) => get<RuntimeResponse>('/api/runtime', signal),
    fetchHistory: (filters = {}, page = {}, signal) => {
      const query = new URLSearchParams(serializeHistoryFilters(filters));
      if (page.limit !== undefined) query.set('limit', String(page.limit));
      if (page.before) {
        query.set('before', page.before.recorded_at);
        query.set('before_id', String(page.before.reading_id));
      }
      const suffix = query.toString() ? `?${query.toString()}` : '';
      return get<HistoryResponse>(`/api/history${suffix}`, signal);
    },
    buildExportURL,
  };
}

export function serializeHistoryFilters(filters: HistoryFilters = {}): string {
  const query = new URLSearchParams();
  if (filters.from) query.set('from', filters.from);
  if (filters.to) query.set('to', filters.to);
  if (filters.location_id) query.set('location_id', filters.location_id);
  if (filters.device_id) query.set('device_id', filters.device_id);
  if (filters.status) query.set('status', filters.status);
  return query.toString();
}

export function buildExportURL(filters: HistoryFilters = {}): string {
  const query = serializeHistoryFilters(filters);
  return pathWithBase(`/api/export.csv${query ? `?${query}` : ''}`);
}

export const apiClient = createApiClient();
