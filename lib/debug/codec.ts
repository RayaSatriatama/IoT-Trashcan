export type DebugLevel = 'ERROR' | 'WARN' | 'INFO' | 'DEBUG';
export type DebugCursor = { boot_id: string; after: string };
export type DebugEvent = { sequence: string; monotonic_us: string; level: DebugLevel; module: number; event_code: number; request_id: number | null; duration_us: number; arg0: number; arg1: number; related_sequence: string | null };
export type DebugMeta = { boot_id: string; build_id: string; level: DebugLevel; oldest_sequence: string; latest_sequence: string; overwritten: string; delivery_lost: string; usb_lost: string; captured_monotonic_us: string; sequence_exhausted: boolean };
export type DebugPage = { boot_id: string; level: DebugLevel; oldest_sequence: string; next_after: string; overwritten: string; gap: boolean; boot_changed: boolean; event: DebugEvent | null };
export type DebugResponse = { kind: 'meta'; meta: DebugMeta } | { kind: 'page'; page: DebugPage } | { kind: 'level'; level: DebugLevel };
const levels: DebugLevel[] = ['ERROR', 'WARN', 'INFO', 'DEBUG'];
const u64 = /^(0|[1-9][0-9]{0,19})$/u;
const u32 = /^(0|[1-9][0-9]{0,9})$/u;
const maxU64 = 18446744073709551615n;
const maxU32 = 4294967295n;
function decimal(s: string, re: RegExp, max: bigint): string {
  if (!re.test(s) || BigInt(s) > max) throw new Error('debug number invalid');
  return s;
}
function registered(module: number, code: number): boolean {
  return (module === 1 && code >= 1 && code <= 3) ||
    (module === 2 && code >= 10 && code <= 16) ||
    (module === 3 && code >= 20 && code <= 25) ||
    (module === 4 && code >= 30 && code <= 32) ||
    (module === 5 && code >= 40 && code <= 42) ||
    (module === 6 && code >= 50 && code <= 53) ||
    (module === 7 && code >= 60 && code <= 62);
}
function smallNumber(s: string, maxDigits: number): number {
  if (!new RegExp(`^(0|[1-9][0-9]{0,${maxDigits - 1}})$`, 'u').test(s)) throw new Error('debug enum invalid');
  return Number(s);
}
function parseLevel(s: string): DebugLevel {
  const level = smallNumber(s, 1);
  if (level > 3) throw new Error('debug level invalid');
  return levels[level];
}
function event(s: string): DebugEvent {
  const f = s.split(',');
  if (f.length !== 10) throw new Error('event arity');
  const sequence = decimal(f[0], u64, maxU64);
  if (sequence === '0') throw new Error('sequence invalid');
  const monotonic_us = decimal(f[1], u64, maxU64);
  const level = parseLevel(f[2]);
  const module = smallNumber(f[3], 1);
  const event_code = smallNumber(f[4], 2);
  if (!registered(module, event_code)) throw new Error('event registry invalid');
  const request_id = f[5] === '-' ? null : Number(decimal(f[5], u32, maxU32));
  const duration_us = Number(decimal(f[6], u32, maxU32));
  const signed = /^(0|-?[1-9][0-9]{0,9})$/u;
  if (!signed.test(f[7]) || !signed.test(f[8])) throw new Error('argument invalid');
  const arg0 = Number(f[7]);
  const arg1 = Number(f[8]);
  if (arg0 < -2147483648 || arg0 > 2147483647 || arg1 < -2147483648 || arg1 > 2147483647) {
    throw new Error('argument range');
  }
  const related_sequence = f[9] === '-' ? null : decimal(f[9], u64, maxU64);
  return { sequence, monotonic_us, level, module, event_code, request_id, duration_us, arg0, arg1, related_sequence };
}
export function decodeDebugResponse(text: string): DebugResponse {
  if (text.length > 512 || !/^[\x20-\x7e]+$/u.test(text)) throw new Error('debug response must be bounded ASCII');
  const f = text.split('|');
  if (f[0] !== 'DBG1') throw new Error('schema invalid');
  if (f[1] === 'L' && f.length === 3) return { kind: 'level', level: parseLevel(f[2]) };
  if (f[1] === 'M' && f.length === 12) {
    if (!/^[0-9a-f]{32}$/u.test(f[2]) || f[3].length % 2 !== 0 || !/^(?:[0-9a-f]{2})*$/u.test(f[3])) {
      throw new Error('meta identity invalid');
    }
    const build_id = String.fromCharCode(...(f[3].match(/../gu) ?? []).map((byte) => Number.parseInt(byte, 16)));
    if (!/^[\x20-\x7e]{1,96}$/u.test(build_id)) throw new Error('build invalid');
    if (f[11] !== '0' && f[11] !== '1') throw new Error('exhaustion flag invalid');
    const oldest_sequence = decimal(f[5], u64, maxU64);
    const latest_sequence = decimal(f[6], u64, maxU64);
    if ((oldest_sequence === '0') !== (latest_sequence === '0') || BigInt(oldest_sequence) > BigInt(latest_sequence)) {
      throw new Error('meta sequence bounds invalid');
    }
    const meta: DebugMeta = {
      boot_id: f[2], build_id, level: parseLevel(f[4]), oldest_sequence, latest_sequence,
      overwritten: decimal(f[7], u64, maxU64), delivery_lost: decimal(f[8], u64, maxU64),
      usb_lost: decimal(f[9], u64, maxU64), captured_monotonic_us: decimal(f[10], u64, maxU64),
      sequence_exhausted: f[11] === '1',
    };
    return { kind: 'meta', meta };
  }
  if (f[1] === 'P' && f.length === 10) {
    if (!/^[0-9a-f]{32}$/u.test(f[2]) || !['0', '1'].includes(f[7]) || !['0', '1'].includes(f[8])) {
      throw new Error('page identity/flags invalid');
    }
    const event_value = f[9] === '-' ? null : event(f[9]);
    const next_after = decimal(f[5], u64, maxU64);
    if (event_value !== null && BigInt(event_value.sequence) > BigInt(next_after)) {
      throw new Error('page cursor bounds invalid');
    }
    const page: DebugPage = {
      boot_id: f[2], level: parseLevel(f[3]), oldest_sequence: decimal(f[4], u64, maxU64),
      next_after, overwritten: decimal(f[6], u64, maxU64), gap: f[7] === '1',
      boot_changed: f[8] === '1', event: event_value,
    };
    return { kind: 'page', page };
  }
  throw new Error('debug response envelope invalid');
}
