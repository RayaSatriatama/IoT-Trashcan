import { describe, expect, it } from 'vitest';

import { ApiError, createApiClient } from './client';
import type { OverviewResponse } from './types';

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
});
