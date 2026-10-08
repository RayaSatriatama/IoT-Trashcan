export const SIGAP_BLE_SERVICE_UUID = '6f7a0001-5a47-4c9f-9e55-534947415001';
export const SIGAP_BLE_CONTROL_UUID = '6f7a0002-5a47-4c9f-9e55-534947415001';
export const SIGAP_BLE_EVENTS_UUID = '6f7a0003-5a47-4c9f-9e55-534947415001';

export const BLE_FRAME_MAX_BYTES = 512;

export type WifiNetwork = {
  ssid: string;
  rssi: number;
  secure: boolean;
};

export type WifiStatus = {
  state: 'SETUP_REQUIRED' | 'CONNECTING' | 'CONNECTED' | 'BACKOFF' | 'UNCONFIGURED';
  ssid: string | null;
};

export type BleResponse = {
  id: string;
  ok: boolean;
  error?: string;
  status?: WifiStatus;
  networks?: WifiNetwork[];
};

export function encodeBleCommand(id: string, command: string, payload: Record<string, unknown> = {}): Uint8Array {
  const body = JSON.stringify({ id, command, ...payload });
  const encoded = new TextEncoder().encode(`${body}\n`);
  if (encoded.byteLength > BLE_FRAME_MAX_BYTES) throw new Error('BLE command is too large');
  return encoded;
}

export function decodeBleResponse(bytes: Uint8Array): BleResponse {
  if (bytes.byteLength > BLE_FRAME_MAX_BYTES) throw new Error('BLE response is too large');
  const text = new TextDecoder().decode(bytes).trim();
  if (!text) throw new Error('BLE response is empty');
  const value: unknown = JSON.parse(text);
  if (!value || typeof value !== 'object') throw new Error('BLE response must be an object');
  const response = value as Record<string, unknown>;
  if (typeof response.id !== 'string' || typeof response.ok !== 'boolean') throw new Error('BLE response envelope is invalid');
  return response as unknown as BleResponse;
}

export function isWebBluetoothAvailable(): boolean {
  return typeof globalThis !== 'undefined' &&
    typeof globalThis.isSecureContext === 'boolean' &&
    globalThis.isSecureContext &&
    typeof (globalThis as unknown as { navigator?: { bluetooth?: unknown } }).navigator?.bluetooth !== 'undefined';
}
