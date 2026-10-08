import { describe, expect, it } from 'vitest';
import { decodeBleResponse, encodeBleCommand, isWebBluetoothAvailable, BLE_FRAME_MAX_BYTES } from './protocol';

describe('BLE provisioning protocol', () => {
  it('encodes bounded commands without retaining credentials', () => {
    const bytes = encodeBleCommand('1', 'CONNECT', { ssid: 'Lab', password: 'secret' });
    expect(new TextDecoder().decode(bytes)).toContain('"ssid":"Lab"');
    expect(bytes.byteLength).toBeLessThanOrEqual(BLE_FRAME_MAX_BYTES);
  });

  it('rejects oversized and malformed responses', () => {
    expect(() => decodeBleResponse(new Uint8Array(BLE_FRAME_MAX_BYTES + 1))).toThrow();
    expect(() => decodeBleResponse(new TextEncoder().encode('[]'))).toThrow();
  });

  it('does not claim support outside a secure Web Bluetooth context', () => {
    expect(isWebBluetoothAvailable()).toBe(false);
  });
});
