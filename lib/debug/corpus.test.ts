import { readFileSync } from 'node:fs';
import { expect, it } from 'vitest';
import { decodeDebugResponse } from './codec';

type Case = { target: string; name: string; raw_hex: string };
const corpus = JSON.parse(readFileSync(new URL('../../contracts/debug/v1/fixtures/invalid.json', import.meta.url), 'utf8')) as {
  format: string; version: number; encoding: string; cases: Case[];
};

it('rejects every exact-byte BLE invalid-corpus case with fatal UTF-8 decoding', () => {
  expect(corpus.format).toBe('sigap-debug-invalid-corpus');
  expect(corpus.version).toBe(1);
  expect(corpus.encoding).toBe('raw bytes as lowercase hexadecimal');
  const names = corpus.cases.map((entry) => entry.name);
  expect(new Set(names).size).toBe(names.length);
  for (const target of ['ble', 'upload', 'ack']) {
    expect(corpus.cases.filter((entry) => entry.target === target).length).toBeGreaterThan(0);
  }
  const cases = corpus.cases.filter((entry) => entry.target === 'ble');
  expect(cases.length).toBeGreaterThan(0);
  for (const entry of cases) {
    expect(entry.raw_hex, entry.name).toMatch(/^(?:[0-9a-f]{2})+$/u);
    const raw = Buffer.from(entry.raw_hex, 'hex');
    expect(() => {
      const text = new TextDecoder('utf-8', { fatal: true }).decode(raw);
      decodeDebugResponse(text);
    }, entry.name).toThrow();
  }
});
