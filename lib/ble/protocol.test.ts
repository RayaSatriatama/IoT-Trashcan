import { describe, expect, it } from 'vitest';
import {
  assembleBleFrames,
  BLE_ATT_FRAME_BYTES,
  BLE_FRAME_MAX_BYTES,
  decodeBleFrame,
  decodeBleResponse,
  encodeBleCommand,
  encodeCalibrationCommand,
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

  it('encodes bounded calibration operations without changing sensor identity', () => {
    const bytes = encodeCalibrationCommand('CRITERIA', 'ds18b20_b3', {
      reference_c: 25.5,
      stable_samples: 8,
    });
    expect(new TextDecoder().decode(bytes)).toBe(
      'CAL CRITERIA ds18b20_b3 reference_c=25.5 stable_samples=8\n',
    );
    expect(new TextDecoder().decode(encodeCalibrationCommand('CAPTURE', 'ds18b20_b3', { anchor: 'reference' })))
      .toBe('CAL CAPTURE ds18b20_b3 reference\n');
  expect(() => encodeCalibrationCommand('START', 'sensor with spaces')).toThrow();
  expect(() => encodeCalibrationCommand('START', 'ds18b20_b3', { note: 'x y' })).toThrow();
  expect(new TextDecoder().decode(encodeCalibrationCommand('START', 'hx711_organic', {
    reference_mass_mg: 2000,
  }))).toContain('CAL START hx711_organic reference_mass_mg=2000');
  expect(new TextDecoder().decode(encodeCalibrationCommand('COMMIT', 'hx711_organic', {
    expected_profile_version: '18446744073709551615',
    expected_profile_hash: 'sha256:' + 'a'.repeat(64),
  }))).toContain('expected_profile_version=18446744073709551615');
});

  it('decodes calibration response text while preserving bounded errors', () => {
    expect(decodeBleResponse(new TextEncoder().encode(
      'CAL1|op=PREVIEW|status=ACCEPTED|session=12|sensor=ds18b20_b3|data=proposal_hash=abc;profile_version=2',
    ))).toEqual({ ok: true, calibration: {
      version: 'CAL1', operation: 'PREVIEW', status: 'ACCEPTED', sessionId: '12',
      sensor: 'ds18b20_b3', data: { proposal_hash: 'abc', profile_version: '2' },
    } });
    expect(decodeBleResponse(new TextEncoder().encode('ERR CAL_SESSION_BUSY')))
      .toEqual({ ok: false, error: 'CAL_SESSION_BUSY' });
    expect(decodeBleResponse(new TextEncoder().encode(
      'CAL1|op=COMMIT|status=ERROR|session=12|sensor=ds18b20_b3|data=error=CONFLICT',
    ))).toEqual({ ok: false, error: 'CONFLICT', calibration: {
      version: 'CAL1', operation: 'COMMIT', status: 'ERROR', sessionId: '12',
      sensor: 'ds18b20_b3', data: { error: 'CONFLICT' }, error: 'CONFLICT',
    } });
  });
});
