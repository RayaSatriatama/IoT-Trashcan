import type {
  CanonicalStatus,
  DeviceDTO,
  LocationDetail,
  LocationListResponse,
  OverviewResponse,
  ResearchResponse,
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
  fetchDevice(id: string, signal?: AbortSignal): Promise<DeviceDTO>;
  fetchResearch(id: string, signal?: AbortSignal): Promise<ResearchResponse>;
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
    fetchDevice: (id, signal) => get<DeviceDTO>(`/api/devices/${encodeId(id)}`, signal),
    fetchResearch: (id, signal) => get<ResearchResponse>(`/api/devices/${encodeId(id)}/research`, signal),
  };
}

export const apiClient = createApiClient();
