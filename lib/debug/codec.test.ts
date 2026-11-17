import { readFileSync } from 'node:fs';
import { expect, it } from 'vitest';
import { decodeDebugResponse } from './codec';
import {
  assembleBleFrames,
  BLE_ATT_FRAME_BYTES,
  BLE_FRAME_MAX_BYTES,
  decodeBleFrame,
  encodeBleFrames,
} from '../ble/protocol';

const fixtureText = readFileSync(new URL('../../contracts/debug/v1/fixtures/ble.txt', import.meta.url), 'ascii');
const fixtures = new Map(fixtureText.split('\n').filter(Boolean).map((line) => {
  const separator = line.indexOf('=');
  return [line.slice(0, separator), line.slice(separator + 1)];
}));
const observationText = readFileSync(new URL('../../contracts/debug/v1/fixtures/ble-observation.txt', import.meta.url), 'ascii');
const observation = new Map(observationText.split('\n').filter(Boolean).map((line) => {
  const separator = line.indexOf('=');
  return [line.slice(0, separator), line.slice(separator + 1)];
}));
const response = (label: string) => {
  const value = fixtures.get(label);
  if (value === undefined) throw new Error(`missing actual producer fixture ${label}`);
  return value;
};
const pageEvent = (text: string) => {
  const value = decodeDebugResponse(text);
  if (value.kind !== 'page') throw new Error('fixture is not a PAGE');
  return value.page;
};
const field = (text: string, index: number, value: string) => {
  const fields = text.split('|');
  fields[index] = value;
  return fields.join('|');
};
const tupleField = (text: string, index: number, value: string) => {
  const fields = text.split('|');
  const tuple = fields[9].split(',');
  tuple[index] = value;
  fields[9] = tuple.join(',');
  return fields.join('|');
};
const maxU64 = '18446744073709551615';

it('decodes every actual C++ BLE producer fixture without changing its bytes', () => {
  expect(decodeDebugResponse('DBG1|L|2')).toEqual({ kind: 'level', level: 'INFO' });
  expect([...fixtures.keys()]).toEqual([
    'META', 'PAGE', 'EMPTY_PAGE', 'MAX_PAGE', 'MAX_META', 'GAP_PAGE', 'PAGE_ZERO',
  ]);
  expect(fixtures.size).toBe(7);
  for (const text of fixtures.values()) expect(() => decodeDebugResponse(text)).not.toThrow();

  const meta = decodeDebugResponse(response('META'));
  expect(meta).toMatchObject({ kind: 'meta', meta: {
    boot_id: '00000000000000000000000000000001', build_id: 'TEST', level: 'INFO',
    oldest_sequence: '1', latest_sequence: '2', captured_monotonic_us: '9007199254740993',
  } });
  const page = pageEvent(response('PAGE'));
  expect(page).toMatchObject({ level: 'INFO', oldest_sequence: '1', next_after: '2', event: {
    sequence: '2', monotonic_us: '9007199254740993', level: 'INFO', request_id: null,
    related_sequence: null,
  } });
  expect(pageEvent(response('EMPTY_PAGE')).event).toBeNull();
  expect(decodeDebugResponse(response('MAX_META'))).toMatchObject({ kind: 'meta', meta: {
    build_id: 'X'.repeat(96), level: 'DEBUG', oldest_sequence: maxU64, latest_sequence: maxU64,
    overwritten: maxU64, delivery_lost: maxU64, usb_lost: maxU64,
    captured_monotonic_us: maxU64, sequence_exhausted: true,
  } });
  expect(pageEvent(response('MAX_PAGE'))).toMatchObject({
    oldest_sequence: maxU64, next_after: maxU64, overwritten: maxU64, event: {
      sequence: maxU64, monotonic_us: maxU64, level: 'DEBUG', request_id: 4294967295,
      duration_us: 4294967295, arg0: -2147483648, arg1: 2147483647,
      related_sequence: maxU64,
    },
  });
  expect(pageEvent(response('GAP_PAGE'))).toMatchObject({ gap: true, boot_changed: true, event: null });
  expect(pageEvent(response('PAGE_ZERO')).event).toMatchObject({ request_id: 0, related_sequence: '0' });
});

it('decodes actual C++ codec BLE reason-3 PAGE captures with absent and zero correlations', () => {
  expect([...observation.keys()]).toEqual([
    'PAGE_10_REQUEST_ZERO', 'PAGE_10_REQUEST_ABSENT',
    'PAGE_11_OPTIONALS_ZERO', 'PAGE_11_OPTIONALS_ABSENT', 'HTTP_BLE_OBSERVATION',
  ]);
  const pages = [...observation.entries()].filter(([label]) => label.startsWith('PAGE_'));
  expect(pages).toHaveLength(4);
  const events = pages.map(([, body]) => {
    const page = pageEvent(body);
    if (page.event === null) throw new Error('missing fixture event');
    return page.event;
  });
  expect(events.map((event) => [event.module, event.event_code, event.arg0, event.arg1]))
    .toEqual([[2, 10, 0, 3], [2, 10, 1, 3], [2, 11, 3, 0], [2, 11, 3, 1]]);
  expect(events[0].request_id).toBe(0);
  expect(events[1].request_id).toBeNull();
  expect(events[2]).toMatchObject({ request_id: 0, related_sequence: '0' });
  expect(events[3]).toMatchObject({ request_id: null, related_sequence: null });
});

it('round-trips every producer body through real F1 and R1 framing at IDs 9 and 10', () => {
  for (const id of ['9', '10']) for (const version of ['F1', 'R1'] as const) {
    for (const text of fixtures.values()) {
      const body = new TextEncoder().encode(text);
      const frames = encodeBleFrames(id, body, version);
      expect(frames.length).toBeGreaterThan(0);
      expect(frames.every((frame) => frame.byteLength <= BLE_ATT_FRAME_BYTES)).toBe(true);
      const decoded = frames.map(decodeBleFrame);
      expect(decoded.map((frame) => frame.requestId)).toEqual(frames.map(() => id));
      expect(decoded.every((frame) => frame.version === version)).toBe(true);
      expect(decoded.every((frame) => frame.chunkCount === frames.length)).toBe(true);
      expect(assembleBleFrames(decoded, version)).toEqual(body);
      expect(decodeDebugResponse(new TextDecoder('ascii', { fatal: true }).decode(assembleBleFrames(decoded, version))))
        .toEqual(decodeDebugResponse(text));
    }
  }
});

it('uses the supported maximum body, header, ID, and chunk-count combinations', () => {
  const body = new TextEncoder().encode('x'.repeat(BLE_FRAME_MAX_BYTES));
  for (const id of ['9', '10', '4294967295']) for (const version of ['F1', 'R1'] as const) {
    if (id === '4294967295') {
      expect(() => encodeBleFrames(id, body, version)).toThrow();
      continue;
    }
    const frames = encodeBleFrames(id, body, version);
    expect(frames.length).toBeLessThanOrEqual(512);
    expect(frames.every((frame) => frame.byteLength <= BLE_ATT_FRAME_BYTES)).toBe(true);
    const decoded = frames.map(decodeBleFrame);
    expect(assembleBleFrames(decoded, version)).toEqual(body);
  }
  for (const version of ['F1', 'R1'] as const) {
    const largestId = '4294967295';
    const frames = encodeBleFrames(largestId, new Uint8Array([0x41]), version);
    expect(decodeBleFrame(frames[0])).toMatchObject({ version, requestId: largestId, chunkCount: 1 });
    expect(() => encodeBleFrames(largestId, new TextEncoder().encode(response('META')), version))
      .toThrow('BLE frame header is too large');
  }
  expect(() => encodeBleFrames('1', new Uint8Array(BLE_FRAME_MAX_BYTES + 1))).toThrow();
  for (const invalid of ['', '00000000000', '1x', '1|2', '10000000000', '4294967296', '9999999999']) {
    expect(() => encodeBleFrames(invalid, new Uint8Array([1]), 'F1')).toThrow();
    expect(() => encodeBleFrames(invalid, new Uint8Array([1]), 'R1')).toThrow();
  }
  expect(() => encodeBleFrames('9', new Uint8Array([1]), 'F2' as 'F1')).toThrow();
  expect(() => decodeBleFrame(new Uint8Array(BLE_ATT_FRAME_BYTES + 1))).toThrow();
  for (const header of [
    'F2|9|0|1|x', 'F1|9|0|0|x', 'F1|9|1|1|x', 'R1|10000000000|0|1|x',
    'F1|4294967296|0|1|x', 'R1|9999999999|0|1|x',
  ]) expect(() => decodeBleFrame(new TextEncoder().encode(header))).toThrow();
  expect(() => decodeBleFrame(new TextEncoder().encode('R1|9|0|513|x'))).toThrow('BLE chunk bounds invalid');
});

it('preserves exact uint64 strings and accepts previously stored DEBUG events at INFO', () => {
  const info = decodeDebugResponse(response('PAGE'));
  expect(info.kind).toBe('page');
  if (info.kind !== 'page') throw new Error('wrong response kind');
  expect(info.page.level).toBe('INFO');
  expect(info.page.event?.level).toBe('INFO');
  const debugAtInfo = field(response('MAX_PAGE'), 3, '2');
  expect(pageEvent(debugAtInfo)).toMatchObject({ level: 'INFO', event: { level: 'DEBUG', monotonic_us: maxU64 } });
  expect(pageEvent(response('PAGE_ZERO')).event?.related_sequence).toBe('0');
  expect(pageEvent(response('PAGE_ZERO')).event?.request_id).toBe(0);
});

it('rejects noncanonical numbers, overflow, invalid flags, and malformed tuples', () => {
  const page = response('MAX_PAGE');
  const meta = response('MAX_META');
  for (const text of [
    field(response('PAGE'), 4, '01'),
    field(response('PAGE'), 4, '-0'),
    field(page, 4, '18446744073709551616'),
    tupleField(page, 0, '0'),
    tupleField(page, 5, '4294967296'),
    tupleField(page, 6, '4294967296'),
    tupleField(page, 7, '-2147483649'),
    tupleField(page, 8, '2147483648'),
    tupleField(page, 5, '00'),
    tupleField(page, 5, '+0'),
    tupleField(page, 2, '4'),
    tupleField(page, 3, '8'),
    tupleField(page, 4, '99'),
    tupleField(page, 7, '-0'),
    tupleField(page, 9, '18446744073709551616'),
    field(response('GAP_PAGE'), 7, '2'),
    field(response('META'), 11, '2'),
    field(meta, 4, '03'),
    field(meta, 5, '18446744073709551616'),
    field(meta, 5, '-0'),
    field(meta, 6, '0'),
    field(page, 5, '1'),
  ]) expect(() => decodeDebugResponse(text)).toThrow();
});

it('requires exact grammar, known registered event pairs, and strict ASCII identities', () => {
  const validPage = response('PAGE');
  for (const text of [
    'DBG1|L|2|', 'DBG1|X|2', 'DBG1|L|02', 'DBG1|L|4', 'DBG1|L|+2',
    `${validPage}|extra`, validPage.replace('DBG1|P|', 'DBG1|P |'),
    field(validPage, 2, 'A'.repeat(32)), field(validPage, 2, 'a'.repeat(31)),
    tupleField(validPage, 2, '4'), tupleField(validPage, 3, '7'), tupleField(validPage, 4, '99'),
    tupleField(validPage, 9, '0,extra'),
    field(response('GAP_PAGE'), 8, '2'),
    field(response('META'), 3, '5445535G'),
    field(response('META'), 3, '5445535'),
    field(response('META'), 3, ''),
    `${response('PAGE')}\n`, `${response('PAGE')}é`, `${response('PAGE')}\u0000`,
  ]) expect(() => decodeDebugResponse(text)).toThrow();
});

it('enforces printable nonempty decoded build identity and 512-byte response bound', () => {
  const meta = response('META');
  const makeBuild = (value: string) => `DBG1|M|${'0'.repeat(32)}|${[...value].map((c) => c.charCodeAt(0).toString(16).padStart(2, '0')).join('')}|2|1|2|0|0|0|1|0`;
  expect(decodeDebugResponse(makeBuild('A'.repeat(96)))).toMatchObject({ kind: 'meta', meta: { build_id: 'A'.repeat(96) } });
  for (const text of [
    makeBuild(''), makeBuild('A'.repeat(97)), makeBuild('A\nB'), makeBuild('é'),
    meta + '|'.repeat(512),
  ]) expect(() => decodeDebugResponse(text)).toThrow();
});

it('validates optional markers independently and distinguishes absent from numeric zero', () => {
  const zero = response('PAGE_ZERO');
  expect(pageEvent(zero).event).toMatchObject({ request_id: 0, related_sequence: '0' });
  expect(pageEvent(response('PAGE')).event).toMatchObject({ request_id: null, related_sequence: null });
  for (const text of [
    tupleField(zero, 5, '+0'),
    tupleField(zero, 9, '-0'),
    tupleField(zero, 9, '00'),
  ]) expect(() => decodeDebugResponse(text)).toThrow();
});
