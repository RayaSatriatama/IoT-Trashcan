import { describe, expect, it } from 'vitest';

import { ApiError, buildExportURL, createApiClient } from './client';
import type { HistoryResponse, OverviewResponse } from './types';

function jsonResponse(body: unknown, init: ResponseInit = {}): Response {
  return new Response(JSON.stringify(body), {
    status: 200,
    headers: { 'content-type': 'application/json' },
    ...init,
  });
}

describe('monitoring API client', () => {
  it('decodes an overview response and forwards the abort signal', async () => {
    const overview: OverviewResponse = {
      total_locations: 1,
      total_devices: 2,
      pickup_required: 1,
      safety_alerts: 0,
      priority_locations: [],
      recent_activity: [],
    };
    const signal = new AbortController().signal;
    let requestUrl = '';
    let requestSignal: AbortSignal | null | undefined;
    const client = createApiClient(async (input, init) => {
      requestUrl = String(input);
      requestSignal = init?.signal;
      return jsonResponse(overview);
    });

    await expect(client.fetchOverview(signal)).resolves.toEqual(overview);
    expect(requestUrl).toBe('/api/overview');
    expect(requestSignal).toBe(signal);
  });

  it('builds encoded location query parameters', async () => {
    let requestUrl = '';
    const client = createApiClient(async (input) => {
      requestUrl = String(input);
      return jsonResponse({ locations: [] });
    });

    await client.fetchLocations({ query: 'Alpha Lab', status: 'Safety Alert' });

    expect(requestUrl).toBe('/api/locations?q=Alpha+Lab&status=Safety+Alert');
  });

  it('fetches the flattened device summary endpoint without location fan-out', async () => {
    let requestUrl = '';
    const client = createApiClient(async (input) => {
      requestUrl = String(input);
      return jsonResponse({ devices: [] });
    });

    await expect(client.fetchDevices()).resolves.toEqual({ devices: [] });
    expect(requestUrl).toBe('/api/devices');
  });

  it('surfaces JSON error text for non-success responses as an ApiError', async () => {
    const client = createApiClient(async () =>
      jsonResponse({ error: 'not found' }, { status: 404 }),
    );

    const request = client.fetchDevice('missing-device');
    await expect(request).rejects.toBeInstanceOf(ApiError);
    await expect(request).rejects.toMatchObject({ status: 404, message: 'not found' });
  });

  it('rejects malformed successful responses as an ApiError', async () => {
    const client = createApiClient(
      async () =>
        new Response('<html>not json</html>', {
          status: 200,
          headers: { 'content-type': 'text/html' },
        }),
    );

    await expect(client.fetchOverview()).rejects.toMatchObject({
      status: 200,
      name: 'ApiError',
    });
  });

  it('serializes history filters and fetches audit rows', async () => {
    let requestUrl = '';
    const historyResponse: HistoryResponse = {
      items: [],
      filters: { from: null, to: null, location_id: null, device_id: null, status: null },
      page: { limit: 50, has_more: false, next: null, total_count: 0 },
      summary: { total: 0, normal: 0, waspada: 0, pickup: 0, safety_alert: 0, data_invalid: 0 },
      trend: {
        scope_from: '2026-08-19T17:00:00Z',
        scope_to: '2026-09-18T17:00:00Z',
        granularity: 'day',
        default_window: true,
        truncated: false,
        buckets: [],
      },
    };
    const client = createApiClient(async (input) => {
      requestUrl = String(input);
      return jsonResponse(historyResponse);
    });

    await expect(client.fetchHistory({
      from: '2026-09-01T00:00:00Z',
      to: '2026-09-18T00:00:00Z',
      location_id: 'loc/1',
      device_id: 'SIGAP 01',
      status: 'Safety Alert',
    })).resolves.toEqual(historyResponse);

    expect(requestUrl).toBe(
      '/api/history?from=2026-09-01T00%3A00%3A00Z&to=2026-09-18T00%3A00%3A00Z&location_id=loc%2F1&device_id=SIGAP+01&status=Safety+Alert',
    );
  });

  it('serializes bounded history cursor pagination separately from export filters', async () => {
    let requestUrl = '';
    const client = createApiClient(async (input) => {
      requestUrl = String(input);
      return jsonResponse({
        items: [],
        filters: { from: null, to: null, location_id: null, device_id: null, status: null },
        page: { limit: 30, has_more: false, next: null, total_count: 0 },
      });
    });

    await client.fetchHistory(
      { location_id: 'loc-1' },
      { limit: 30, before: { recorded_at: '2026-09-17T10:00:00Z', reading_id: 42 } },
    );

    expect(requestUrl).toBe(
      '/api/history?location_id=loc-1&limit=30&before=2026-09-17T10%3A00%3A00Z&before_id=42',
    );
    expect(buildExportURL({ location_id: 'loc-1' })).toBe('/api/export.csv?location_id=loc-1');
  });

  it('builds an export URL from the same history filter contract', () => {
    expect(buildExportURL({ location_id: 'loc-1', status: 'Waspada' })).toBe(
      '/api/export.csv?location_id=loc-1&status=Waspada',
    );
  });

  it('reads safe runtime simulation metadata without credentials', async () => {
    let requestUrl = '';
    const client = createApiClient(async (input) => {
      requestUrl = String(input);
      return jsonResponse({ simulation: true, simulationIndicator: false });
    });

    await expect(client.fetchRuntime()).resolves.toEqual({ simulation: true, simulationIndicator: false });
    expect(requestUrl).toBe('/api/runtime');
  });
});
