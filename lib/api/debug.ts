import { ApiError, type Fetcher } from './client';
import type { OperatorAuthorization } from './calibration';
import type { DebugLevel } from '../debug/types';

export type DebugHistoryEvent = {
  sequence: string;
  monotonic_us: string;
  level: DebugLevel;
  module: number;
  event_code: number;
  request_id: number | null;
  duration_us: number;
  arg0: number;
  arg1: number;
  related_sequence: string | null;
  ingestion_id: string;
  received_at: string;
  boot_id: string;
  build_id: string;
  snapshot_level: DebugLevel;
  captured_monotonic_us: string;
  oldest_sequence: string;
  overwritten: string;
  delivery_lost: string;
};

export type DebugHistoryPage = {
  device_id: string;
  events: DebugHistoryEvent[];
  next_after: string;
  oldest_available: string;
  retention_gap: boolean;
  has_more: boolean;
};

const modules: Record<number, number> = {
  1: 1, 2: 1, 3: 1, 10: 2, 11: 2, 12: 2, 13: 2, 14: 2, 15: 2, 16: 2,
  20: 3, 21: 3, 22: 3, 23: 3, 24: 3, 25: 3, 30: 4, 31: 4, 32: 4,
  40: 5, 41: 5, 42: 5, 50: 6, 51: 6, 52: 6, 53: 6, 60: 7, 61: 7, 62: 7,
};
const u64 = 18446744073709551615n;
const i64 = 9223372036854775807n;
const u32 = 4294967295;
const levels = new Set(['ERROR', 'WARN', 'INFO', 'DEBUG']);

function obj(value: unknown, keys: string[], name: string): Record<string, unknown> {
  if (!value || typeof value !== 'object' || Array.isArray(value)) throw Error(name);
  const object = value as Record<string, unknown>;
  if (Object.keys(object).length !== keys.length || keys.some(key => !Object.hasOwn(object, key))) throw Error(name);
  return object;
}

function str(value: unknown, name: string, max?: number): string {
  if (typeof value !== 'string' || !value || max !== undefined && value.length > max) throw Error(name);
  return value;
}

function dec(value: unknown, name: string, max: bigint, positive = false, maxDigits = 20): string {
  const text = str(value, name, maxDigits);
  if (!/^(0|[1-9][0-9]*)$/.test(text)) throw Error(name);
  const integer = BigInt(text);
  if (integer > max || positive && integer === 0n) throw Error(name);
  return text;
}

function num(value: unknown, name: string, min: number, max: number): number {
  if (typeof value !== 'number' || !Number.isInteger(value) || value < min || value > max) throw Error(name);
  return value;
}

function lvl(value: unknown, name: string): DebugLevel {
  if (typeof value !== 'string' || !levels.has(value)) throw Error(name);
  return value as DebugLevel;
}

function decode(value: unknown, device: string, after: string, limit: number): DebugHistoryPage {
  const page = obj(value, ['device_id', 'events', 'next_after', 'oldest_available', 'retention_gap', 'has_more'], 'page');
  if (str(page.device_id, 'device', 64) !== device || !Array.isArray(page.events) || page.events.length > limit || typeof page.retention_gap !== 'boolean' || typeof page.has_more !== 'boolean') throw Error('page');
  const next = dec(page.next_after, 'next_after', i64);
  const oldest = dec(page.oldest_available, 'oldest_available', i64);
  if (page.events.length) {
    const firstIngestion = dec((page.events[0] as Record<string, unknown>)?.ingestion_id, 'ingestion_id', i64, true, 19);
    if (BigInt(oldest) === 0n || BigInt(oldest) > BigInt(firstIngestion)) throw Error('oldest_available');
  }
  const events = page.events.map(value => {
    const event = obj(value, ['sequence', 'monotonic_us', 'level', 'module', 'event_code', 'request_id', 'duration_us', 'arg0', 'arg1', 'related_sequence', 'ingestion_id', 'received_at', 'boot_id', 'build_id', 'snapshot_level', 'captured_monotonic_us', 'oldest_sequence', 'overwritten', 'delivery_lost'], 'event');
    const module = num(event.module, 'module', 0, 255);
    const eventCode = num(event.event_code, 'event_code', 0, 65535);
    if (modules[eventCode] !== module) throw Error('registry');
    if (event.request_id !== null) num(event.request_id, 'request_id', 0, u32);
    if (event.related_sequence !== null) dec(event.related_sequence, 'related_sequence', u64);
    const boot = str(event.boot_id, 'boot_id');
    if (!/^[0-9a-f]{32}$/.test(boot)) throw Error('boot_id');
    const build = str(event.build_id, 'build_id', 96);
    if ([...build].some(char => char < ' ' || char > '~')) throw Error('build_id');
    const received = str(event.received_at, 'received_at');
    if (!/^\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d(?:\.\d+)?(?:Z|[+-]\d\d:\d\d)$/.test(received) || !Number.isFinite(Date.parse(received))) throw Error('received_at');
    const sequence = dec(event.sequence, 'sequence', u64, true);
    const monotonic = dec(event.monotonic_us, 'monotonic_us', u64);
    const captured = dec(event.captured_monotonic_us, 'captured_monotonic_us', u64);
    if (BigInt(captured) < BigInt(monotonic)) throw Error('captured_monotonic_us');
    return {
      sequence,
      monotonic_us: monotonic,
      level: lvl(event.level, 'level'),
      module,
      event_code: eventCode,
      request_id: event.request_id as number | null,
      duration_us: num(event.duration_us, 'duration_us', 0, u32),
      arg0: num(event.arg0, 'arg0', -2147483648, 2147483647),
      arg1: num(event.arg1, 'arg1', -2147483648, 2147483647),
      related_sequence: event.related_sequence as string | null,
      ingestion_id: dec(event.ingestion_id, 'ingestion_id', i64, true),
      received_at: received,
      boot_id: boot,
      build_id: build,
      snapshot_level: lvl(event.snapshot_level, 'snapshot_level'),
      captured_monotonic_us: captured,
      oldest_sequence: dec(event.oldest_sequence, 'oldest_sequence', u64),
      overwritten: dec(event.overwritten, 'overwritten', u64),
      delivery_lost: dec(event.delivery_lost, 'delivery_lost', u64),
    };
  });
  let previous = BigInt(after);
  for (const event of events) {
    const ingestion = BigInt(event.ingestion_id);
    if (ingestion <= previous) throw Error('order');
    previous = ingestion;
  }
  if (events.length && (BigInt(next) !== previous || page.has_more && events.length !== limit) || !events.length && (next !== after || page.has_more)) throw Error('cursor');
  return { device_id: device, events, next_after: next, oldest_available: oldest, retention_gap: page.retention_gap, has_more: page.has_more };
}

function defaultBase(): string {
  return typeof process !== 'undefined' && process.env.EXPO_PUBLIC_API_BASE_URL
    ? process.env.EXPO_PUBLIC_API_BASE_URL.replace(/\/$/u, '')
    : '';
}

export function createDebugApi(auth: OperatorAuthorization, fetcher: Fetcher = fetch, baseUrl = defaultBase()) {
  if (typeof auth?.bearerToken !== 'string' || !auth.bearerToken.trim()) throw new Error('Operator authorization is required');
  const base = baseUrl.replace(/\/$/u, '');
  const abortError = () => new DOMException('Aborted', 'AbortError');

  return {
    history: async (deviceId: string, after: string, limit: number, signal?: AbortSignal): Promise<DebugHistoryPage> => {
      if (typeof deviceId !== 'string' || !deviceId.trim() || deviceId.length > 64 || /[\u0000-\u001f\u007f]/u.test(deviceId)) throw Error('Invalid device id');
      dec(after, 'after', i64);
      if (!Number.isInteger(limit) || limit < 1 || limit > 100) throw Error('Invalid debug history limit');
      if (signal?.aborted) throw abortError();
      const query = new URLSearchParams({ after, limit: String(limit) });
      const response = await fetcher(`${base}/api/device-debug/events/${encodeURIComponent(deviceId)}?${query}`, {
        method: 'GET',
        headers: { Accept: 'application/json', Authorization: `Bearer ${auth.bearerToken}` },
        signal,
      });
      if (signal?.aborted) throw abortError();
      const raw = await response.text();
      if (signal?.aborted) throw abortError();
      let body: unknown;
      try {
        body = raw ? JSON.parse(raw) : null;
      } catch {
        throw new ApiError('Server returned malformed JSON', response.status);
      }
      if (!response.ok) {
        const message = typeof body === 'object' && body !== null && 'error' in body && typeof body.error === 'string'
          ? body.error
          : response.statusText || `Request failed with status ${response.status}`;
        throw new ApiError(message, response.status);
      }
      try {
        return decode(body, deviceId, after, limit);
      } catch {
        throw new ApiError('Server returned invalid debug history', response.status);
      }
    },
  };
}

