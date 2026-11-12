import { describe, expect, it, vi } from 'vitest';
import fixture from '../../tests/fixtures/debug-history-http.json';
import { type Fetcher } from './client';
import { createDebugApi } from './debug';

const auth = { bearerToken: 'opaque operator token' };
const ok = (body: unknown = fixture) => new Response(JSON.stringify(body), {
  status: 200,
  headers: { 'Content-Type': 'application/json' },
});
const copyFixture = () => JSON.parse(JSON.stringify(fixture));
const invalidCases: Array<[string, (x: any) => unknown]> = [
  ['wrong device', (x: any) => ({ ...x, device_id: 'OTHER' })],
  ['wrong oldest retained id', (x: any) => ({ ...x, oldest_available: '3' })],
  ['missing nullable field', (x: any) => ({ ...x, events: [{ ...x.events[0], request_id: undefined }] })],
  ['unknown field', (x: any) => ({ ...x, unexpected: true })],
  ['bad level', (x: any) => ({ ...x, events: [{ ...x.events[0], level: 'TRACE' }] })],
  ['registry mismatch', (x: any) => ({ ...x, events: [{ ...x.events[0], module: 2, event_code: 1 }] })],
  ['uint64 overflow', (x: any) => ({ ...x, events: [{ ...x.events[0], sequence: '18446744073709551616' }] })],
  ['unsafe source precision altered', (x: any) => ({ ...x, events: [{ ...x.events[0], sequence: 2 }] })],
  ['bad server cursor', (x: any) => ({ ...x, next_after: '9223372036854775808' })],
  ['overlong server cursor', (x: any) => ({ ...x, next_after: '9'.repeat(10000) })],
  ['out of range arg', (x: any) => ({ ...x, events: [{ ...x.events[0], arg0: 2147483648 }] })],
  ['overlong decimal is bounded', (x: any) => ({ ...x, events: [{ ...x.events[0], sequence: '9'.repeat(10000) }] })],
  ['overlong monotonic field', (x: any) => ({ ...x, events: [{ ...x.events[0], monotonic_us: '9'.repeat(10000) }] })],
  ['overlong related source field', (x: any) => ({ ...x, events: [{ ...x.events[0], related_sequence: '9'.repeat(10000) }] })],
  ['overlong ingestion id', (x: any) => ({ ...x, events: [{ ...x.events[0], ingestion_id: '9'.repeat(10000) }] })],
  ['row is not after requested cursor', (x: any) => ({ ...x, events: [{ ...x.events[0], ingestion_id: '2' }], next_after: '2' })],
  ['rows are not ascending', (x: any) => ({ ...x, events: [{ ...x.events[0], ingestion_id: '4' }, { ...x.events[0], ingestion_id: '3' }], next_after: '3' })],
  ['next cursor mismatches last row', (x: any) => ({ ...x, next_after: '4' })],
  ['page exceeds requested limit', (x: any) => ({ ...x, events: Array(2).fill(x.events[0]) })],
  ['has_more page is not full', (x: any) => ({ ...x, has_more: true })],
];

describe('debug history API', () => {
  it('requires nonblank authorization before fetch', () => {
    const fetcher = vi.fn();
    expect(() => createDebugApi({ bearerToken: '  ' }, fetcher)).toThrow();
    expect(fetcher).not.toHaveBeenCalled();
  });

  it('GETs the encoded requested device and preserves cursor, auth, and signal', async () => {
    const body = copyFixture();
    body.device_id = 'dev /1';
    const fetcher = vi.fn<Fetcher>().mockResolvedValue(ok(body));
    const controller = new AbortController();
    const api = createDebugApi(auth, fetcher, 'https://api.example/');

    const page = await api.history('dev /1', '0', 25, controller.signal);

    expect(page.events[0].sequence).toBe('2');
    expect(fetcher).toHaveBeenCalledTimes(1);
    const [url, init] = fetcher.mock.calls[0];
    expect(url).toBe('https://api.example/api/device-debug/events/dev%20%2F1?after=0&limit=25');
    expect(init?.method).toBe('GET');
    expect(init?.headers).toEqual({ Accept: 'application/json', Authorization: 'Bearer opaque operator token' });
    expect(init?.signal).toBe(controller.signal);
    expect(init).not.toHaveProperty('body');
  });

  it('decodes actual handler fixture while preserving null and large integer strings', async () => {
    const api = createDebugApi(auth, vi.fn().mockResolvedValue(ok()));
    const page = await api.history('TEST-DEVICE', '0', 100);
    expect(page).toEqual(fixture);
    expect(page.events[0].request_id).toBeNull();
    expect(page.events[0].monotonic_us).toBe('9007199254740993');
  });

  it('accepts present zero values and a registered module/code pair', async () => {
    const body = copyFixture();
    body.events[0].request_id = 0;
    body.events[0].related_sequence = '0';
    body.events[0].module = 1;
    body.events[0].event_code = 2;
    const page = await createDebugApi(auth, vi.fn().mockResolvedValue(ok(body)))
      .history('TEST-DEVICE', '0', 100);
    expect(page.events[0]).toMatchObject({ request_id: 0, related_sequence: '0', module: 1, event_code: 2 });
  });

  it('rejects invalid request values without sending', async () => {
    const fetcher = vi.fn().mockResolvedValue(ok());
    const api = createDebugApi(auth, fetcher);
    for (const args of [['', '0', 1], ['device', '01', 1], ['device', '0', 0], ['device', '9223372036854775808', 1], ['device', 0 as any, 1], ['device', '0', 1.5], ['device', '0', 101], ['device', '0', NaN]] as [string, string, number][]) {
      await expect(api.history(args[0], args[1], args[2])).rejects.toThrow();
    }
    expect(fetcher).not.toHaveBeenCalled();
  });

  it.each(invalidCases)('rejects %s', async (_name, mutate) => {
    const bad = mutate(copyFixture());
    const api = createDebugApi(auth, vi.fn().mockResolvedValue(ok(bad)));
    await expect(api.history('TEST-DEVICE', _name === 'row is not after requested cursor' ? '2' : '0', 100)).rejects.toMatchObject({ name: 'ApiError', status: 200 });
  });

  it('binds rows to an after cursor above JavaScript safe integer precision', async () => {
    const after = '9007199254740993';
    const body = copyFixture();
    body.events[0].ingestion_id = '9007199254740994';
    body.next_after = '9007199254740994';
    body.oldest_available = '1';
    const page = await createDebugApi(auth, vi.fn().mockResolvedValue(ok(body)))
      .history('TEST-DEVICE', after, 1);
    expect(page.next_after).toBe('9007199254740994');
    expect(page.oldest_available).toBe('1');
  });

  it('accepts empty pages with retained minimum before cursor and valid retention gap', async () => {
    const after = '9007199254740993';
    const empty = { ...fixture, events: [], next_after: after, oldest_available: '1', retention_gap: true };
    const page = await createDebugApi(auth, vi.fn().mockResolvedValue(ok(empty)))
      .history('TEST-DEVICE', after, 1);
    expect(page).toEqual(empty);
  });

  it('accepts maximum uint64 source values and exact bounded integer extremes', async () => {
    const body = copyFixture();
    Object.assign(body.events[0], {
      sequence: '18446744073709551615', monotonic_us: '18446744073709551615',
      captured_monotonic_us: '18446744073709551615', oldest_sequence: '18446744073709551615',
      overwritten: '18446744073709551615', delivery_lost: '18446744073709551615',
      related_sequence: '18446744073709551615', ingestion_id: '9223372036854775807',
      duration_us: 4294967295, request_id: 4294967295, arg0: -2147483648, arg1: 2147483647,
    });
    body.next_after = '9223372036854775807';
    const page = await createDebugApi(auth, vi.fn().mockResolvedValue(ok(body)))
      .history('TEST-DEVICE', '9223372036854775806', 1);
    expect(page.events[0]).toMatchObject({ sequence: '18446744073709551615', related_sequence: '18446744073709551615', duration_us: 4294967295, arg0: -2147483648, arg1: 2147483647 });
  });

  it('rejects capture time earlier than event monotonic time', async () => {
    const body = copyFixture();
    body.events[0].captured_monotonic_us = '9007199254740992';
    await expect(createDebugApi(auth, vi.fn().mockResolvedValue(ok(body)))
      .history('TEST-DEVICE', '0', 100)).rejects.toMatchObject({ name: 'ApiError', status: 200 });
  });

  it('accepts oldest source sequence greater than an event sequence', async () => {
    const body = copyFixture();
    body.events[0].oldest_sequence = '3';
    const page = await createDebugApi(auth, vi.fn().mockResolvedValue(ok(body)))
      .history('TEST-DEVICE', '0', 100);
    expect(page.events[0].oldest_sequence).toBe('3');
  });

  it('accepts an empty page when requesting the exact maximum signed cursor', async () => {
    const after = '9223372036854775807';
    const empty = { ...fixture, events: [], next_after: after, oldest_available: after };
    const fetcher = vi.fn<Fetcher>().mockResolvedValue(ok(empty));
    const page = await createDebugApi(auth, fetcher).history('TEST-DEVICE', after, 100);
    expect(page).toEqual(empty);
    expect(fetcher.mock.calls[0][0]).toContain(`?after=${after}&limit=100`);
  });

  it('rejects an already-aborted request without fetching', async () => {
    const controller = new AbortController();
    controller.abort();
    const fetcher = vi.fn<Fetcher>().mockResolvedValue(ok());
    await expect(createDebugApi(auth, fetcher).history('TEST-DEVICE', '0', 1, controller.signal))
      .rejects.toMatchObject({ name: 'AbortError' });
    expect(fetcher).not.toHaveBeenCalled();
  });

  it('cancels while fetch is pending and rejects after fetch resolves', async () => {
    const controller = new AbortController();
    let release!: (response: Response) => void;
    let markStarted!: () => void;
    const started = new Promise<void>(resolve => { markStarted = resolve; });
    const pending = new Promise<Response>(resolve => { release = resolve; });
    const fetcher = vi.fn<Fetcher>(() => { markStarted(); return pending; });
    const request = createDebugApi(auth, fetcher).history('TEST-DEVICE', '0', 1, controller.signal);
    await started;
    controller.abort();
    release(ok());
    await expect(request).rejects.toMatchObject({ name: 'AbortError' });
    expect(fetcher).toHaveBeenCalledTimes(1);
  });

  it('cancels while the body is pending before malformed JSON is parsed', async () => {
    const controller = new AbortController();
    let release!: (body: string) => void;
    let markStarted!: () => void;
    const started = new Promise<void>(resolve => { markStarted = resolve; });
    const pending = new Promise<string>(resolve => { release = resolve; });
    const response = { ok: true, status: 200, statusText: '', text: () => { markStarted(); return pending; } } as Response;
    const fetcher = vi.fn<Fetcher>().mockResolvedValue(response);
    const request = createDebugApi(auth, fetcher).history('TEST-DEVICE', '0', 1, controller.signal);
    await started;
    controller.abort();
    release('{malformed late body');
    await expect(request).rejects.toMatchObject({ name: 'AbortError' });
    expect(fetcher).toHaveBeenCalledTimes(1);
  });

  it('uses ApiError conventions for HTTP and malformed JSON, one request each', async () => {
    const fetcher = vi.fn<Fetcher>()
      .mockResolvedValueOnce(new Response('{"error":"unauthorized"}', { status: 401 }))
      .mockResolvedValueOnce(new Response('{', { status: 200 }));
    const api = createDebugApi(auth, fetcher);
    await expect(api.history('TEST-DEVICE', '0', 1))
      .rejects.toMatchObject({ name: 'ApiError', status: 401, message: 'unauthorized' });
    await expect(api.history('TEST-DEVICE', '0', 1))
      .rejects.toEqual(expect.objectContaining({ name: 'ApiError', status: 200, message: 'Server returned malformed JSON' }));
    expect(fetcher).toHaveBeenCalledTimes(2);
    expect(fetcher.mock.results.every(result => result.type === 'return')).toBe(true);
  });
});
