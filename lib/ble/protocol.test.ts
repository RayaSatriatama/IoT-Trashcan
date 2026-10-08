import { describe, expect, it } from 'vitest';
import {
  assembleBleFrames,
  BLE_ATT_FRAME_BYTES,
  BLE_FRAME_MAX_BYTES,
  decodeBleFrame,
  decodeBleResponse,
  encodeBleCommand,
  encodeBleFrames,
  isWebBluetoothAvailable,
} from './protocol';

describe('BLE provisioning protocol', () => {
  it('encodes bounded commands without retaining credentials', () => {
    const bytes = encodeBleCommand('1', 'CONNECT', { ssid: 'Lab', password: 'secret' });
    expect(new TextDecoder().decode(bytes)).toBe('CONNECT_WIFI Lab secret\n');
    expect(bytes.byteLength).toBeLessThanOrEqual(BLE_FRAME_MAX_BYTES);
  });

  it('frames requests within the ATT payload and preserves UTF-8 payload bytes', () => {
    const logical = encodeBleCommand('ignored', 'CONNECT', { ssid: 'Lab', password: 'x'.repeat(64) });
    const frames = encodeBleFrames('7', logical);
    expect(frames.length).toBeGreaterThan(1);
    expect(frames.every((frame) => frame.byteLength <= BLE_ATT_FRAME_BYTES)).toBe(true);
    const parsed = frames.map(decodeBleFrame);
    expect(parsed.every((frame) => frame.requestId === '7')).toBe(true);
    expect(new TextDecoder().decode(assembleBleFrames(parsed, 'F1'))).toBe(`CONNECT_WIFI Lab ${'x'.repeat(64)}\n`);
  });

  it('rejects reordered, duplicate, or mismatched response chunks', () => {
    const logical = new TextEncoder().encode(`OK SCAN READY\n${'a'.repeat(80)}`);
    const parsed = encodeBleFrames('1', logical, 'R1').map(decodeBleFrame);
    expect(assembleBleFrames(parsed, 'R1')).toEqual(logical);
    expect(() => assembleBleFrames([{ ...parsed[0], requestId: '2' }, ...parsed.slice(1)])).toThrow();
    expect(() => assembleBleFrames([parsed[1], parsed[1], ...parsed.slice(2)])).toThrow();
  });

  it('rejects oversized and malformed responses', () => {
    expect(() => decodeBleResponse(new Uint8Array(BLE_FRAME_MAX_BYTES + 1))).toThrow();
    expect(() => decodeBleResponse(new TextEncoder().encode('[]'))).toThrow();
    expect(decodeBleResponse(new TextEncoder().encode('OK SCAN PENDING')).scanPending).toBe(true);
    expect(decodeBleResponse(new TextEncoder().encode('OK SCAN READY\nLab,-42,1')).networks?.[0].secure).toBe(true);
  });

  it('does not claim support outside a secure Web Bluetooth context', () => {
    expect(isWebBluetoothAvailable()).toBe(false);
  });
});
