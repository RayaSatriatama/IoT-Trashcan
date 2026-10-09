import { afterEach, describe, expect, it } from 'vitest';

import {
  assembleBleFrames,
  decodeBleFrame,
  encodeBleFrames,
  type BleFrame,
} from './protocol';
import { BleProvisioningClient } from './client';

type Listener = (event: { target?: { value?: DataView } }) => void;

class FakeCharacteristic {
  readonly writes: Uint8Array[] = [];
  private readonly listeners: Listener[] = [];
  private readonly onRequest: (frames: Uint8Array[]) => void;
  private requestFrames: Uint8Array[] = [];

  constructor(onRequest: (frames: Uint8Array[]) => void = () => {}) {
    this.onRequest = onRequest;
  }

  async writeValue(value: BufferSource): Promise<void> {
    const bytes = value instanceof ArrayBuffer ? new Uint8Array(value) : new Uint8Array(value.buffer, value.byteOffset, value.byteLength);
    const copy = bytes.slice();
    this.writes.push(copy);
    this.requestFrames.push(copy);
    const parsed = decodeBleFrame(copy);
    if (this.requestFrames.length === parsed.chunkCount) {
      const complete = this.requestFrames;
      this.requestFrames = [];
      this.onRequest(complete);
    }
  }

  async startNotifications(): Promise<FakeCharacteristic> { return this; }

  addEventListener(_event: string, listener: Listener): void { this.listeners.push(listener); }

  emit(frame: Uint8Array): void {
    const value = new DataView(frame.buffer, frame.byteOffset, frame.byteLength);
    this.listeners.forEach((listener) => listener({ target: { value } }));
  }
}

class FakeServer {
  constructor(private readonly control: FakeCharacteristic, private readonly events: FakeCharacteristic) {}
  async getPrimaryService(_uuid: string): Promise<{ getCharacteristic: (uuid: string) => Promise<FakeCharacteristic> }> {
    return { getCharacteristic: async (uuid: string) => uuid.includes('0002-') ? this.control : this.events };
  }
}

const originalNavigator = Object.getOwnPropertyDescriptor(globalThis, 'navigator');
const originalSecureContext = Object.getOwnPropertyDescriptor(globalThis, 'isSecureContext');

afterEach(() => {
  if (originalNavigator) Object.defineProperty(globalThis, 'navigator', originalNavigator);
  else Reflect.deleteProperty(globalThis, 'navigator');
  if (originalSecureContext) Object.defineProperty(globalThis, 'isSecureContext', originalSecureContext);
  else Reflect.deleteProperty(globalThis, 'isSecureContext');
});

describe('BLE provisioning client', () => {
  it('writes framed chunks and waits for persisted CONNECTED status', async () => {
    let statusRequests = 0;
    let control: FakeCharacteristic;
    let events: FakeCharacteristic;
    control = new FakeCharacteristic((requestFrames) => {
      const first = decodeBleFrame(requestFrames[0]);
      const command = new TextDecoder().decode(assembleBleFrames(requestFrames.map(decodeBleFrame), 'F1')).trim();
      let response = 'OK PONG';
      if (command.startsWith('CONNECT_WIFI ')) response = 'OK CONNECTING';
      if (command === 'GET_STATUS') {
        statusRequests += 1;
        response = statusRequests === 1
          ? 'OK STATUS state=CONNECTING ssid='
          : 'OK STATUS state=CONNECTED ssid=TestNetwork';
      }
      encodeBleFrames(first.requestId, new TextEncoder().encode(response), 'R1').forEach((frame) => events.emit(frame));
    });
    events = new FakeCharacteristic();
    const server = new FakeServer(control, events);
    Object.defineProperty(globalThis, 'isSecureContext', { configurable: true, value: true });
    Object.defineProperty(globalThis, 'navigator', {
      configurable: true,
      value: {
        bluetooth: {
          requestDevice: async () => ({
            gatt: { connect: async () => server },
            addEventListener: () => {},
          }),
        },
      },
    });

    const client = new BleProvisioningClient({ connectPollIntervalMs: 0, sleep: async () => {} });
    await client.connect();
    const status = await client.connectWifi('TestNetwork', 'test-password');

    expect(status).toEqual({ state: 'CONNECTED', ssid: 'TestNetwork' });
    expect(control.writes.length).toBeGreaterThan(1);
    expect(control.writes.every((frame) => frame.byteLength <= 20)).toBe(true);
    const requestIds = new Set(control.writes.map((frame) => decodeBleFrame(frame).requestId));
    expect(requestIds.size).toBe(3);
  });

  it('does not treat CONNECTING as a successful provisioning result', async () => {
    let control: FakeCharacteristic;
    let events: FakeCharacteristic;
    control = new FakeCharacteristic((requestFrames) => {
      const first = decodeBleFrame(requestFrames[0]);
      const command = new TextDecoder().decode(assembleBleFrames(requestFrames.map(decodeBleFrame), 'F1')).trim();
      const response = command.startsWith('CONNECT_WIFI ')
        ? 'OK CONNECTING'
        : 'OK STATUS state=BACKOFF ssid=';
      encodeBleFrames(first.requestId, new TextEncoder().encode(response), 'R1').forEach((frame) => events.emit(frame));
    });
    events = new FakeCharacteristic();
    const server = new FakeServer(control, events);
    Object.defineProperty(globalThis, 'isSecureContext', { configurable: true, value: true });
    Object.defineProperty(globalThis, 'navigator', {
      configurable: true,
      value: { bluetooth: { requestDevice: async () => ({ gatt: { connect: async () => server }, addEventListener: () => {} }) } },
    });

    const client = new BleProvisioningClient({ connectTimeoutMs: 1, connectPollIntervalMs: 0, sleep: async () => {} });
    await client.connect();
    await expect(client.connectWifi('TestNetwork', 'test-password')).rejects.toThrow('Koneksi Wi-Fi tidak selesai');
  });

  it('reassembles an asynchronous scan response after the pending acknowledgement', async () => {
    let control: FakeCharacteristic;
    let events: FakeCharacteristic;
    control = new FakeCharacteristic((requestFrames) => {
      const first = decodeBleFrame(requestFrames[0]);
      const command = new TextDecoder().decode(assembleBleFrames(requestFrames.map(decodeBleFrame), 'F1')).trim();
      if (command !== 'SCAN_WIFI') return;
      encodeBleFrames(first.requestId, new TextEncoder().encode('OK SCAN PENDING'), 'R1').forEach((frame) => events.emit(frame));
      encodeBleFrames(first.requestId, new TextEncoder().encode('OK SCAN READY count=1\nTestNetwork,-39,1'), 'R1').forEach((frame) => events.emit(frame));
    });
    events = new FakeCharacteristic();
    const server = new FakeServer(control, events);
    Object.defineProperty(globalThis, 'isSecureContext', { configurable: true, value: true });
    Object.defineProperty(globalThis, 'navigator', {
      configurable: true,
      value: { bluetooth: { requestDevice: async () => ({ gatt: { connect: async () => server }, addEventListener: () => {} }) } },
    });

    const client = new BleProvisioningClient({ requestTimeoutMs: 100, sleep: async () => {} });
    await client.connect();
    await expect(client.scan()).resolves.toEqual([{ ssid: 'TestNetwork', rssi: -39, secure: true }]);
  });

  it('routes calibration commands through the authenticated bounded BLE session', async () => {
    let control: FakeCharacteristic;
    let events: FakeCharacteristic;
    control = new FakeCharacteristic((requestFrames) => {
      const first = decodeBleFrame(requestFrames[0]);
      const command = new TextDecoder().decode(assembleBleFrames(requestFrames.map(decodeBleFrame), 'F1')).trim();
      const response = command === 'CAL STATUS ds18b20_b3'
        ? 'CAL1|op=STATUS|status=ACCEPTED|session=7|sensor=ds18b20_b3|data=state=IDLE'
        : 'CAL1|op=STATUS|status=ERROR|session=7|sensor=ds18b20_b3|data=error=CAL_REJECTED';
      encodeBleFrames(first.requestId, new TextEncoder().encode(response), 'R1').forEach((frame) => events.emit(frame));
    });
    events = new FakeCharacteristic();
    const server = new FakeServer(control, events);
    Object.defineProperty(globalThis, 'isSecureContext', { configurable: true, value: true });
    Object.defineProperty(globalThis, 'navigator', {
      configurable: true,
      value: { bluetooth: { requestDevice: async () => ({ gatt: { connect: async () => server }, addEventListener: () => {} }) } },
    });

    const client = new BleProvisioningClient();
    await client.connect();
    await expect(client.calibrationStatus('ds18b20_b3')).resolves.toEqual({
      version: 'CAL1', operation: 'STATUS', status: 'ACCEPTED', sessionId: '7',
      sensor: 'ds18b20_b3', data: { state: 'IDLE' },
    });
    expect(control.writes.every((frame) => frame.byteLength <= 20)).toBe(true);
  });

  it('delivers first-time calibration setup parameters with START', async () => {
    let control: FakeCharacteristic;
    let events: FakeCharacteristic;
    control = new FakeCharacteristic((requestFrames) => {
      const first = decodeBleFrame(requestFrames[0]);
      const command = new TextDecoder().decode(assembleBleFrames(requestFrames.map(decodeBleFrame), 'F1')).trim();
      const response = command.startsWith('CAL START hx711_organic reference_mass_mg=2000000 tare_counts=12')
        ? 'CAL1|op=START|status=ACCEPTED|session=9|sensor=hx711_organic|data=state=CAPTURING'
        : 'ERR CAL_REJECTED';
      encodeBleFrames(first.requestId, new TextEncoder().encode(response), 'R1').forEach((frame) => events.emit(frame));
    });
    events = new FakeCharacteristic();
    const server = new FakeServer(control, events);
    Object.defineProperty(globalThis, 'isSecureContext', { configurable: true, value: true });
    Object.defineProperty(globalThis, 'navigator', {
      configurable: true,
      value: { bluetooth: { requestDevice: async () => ({ gatt: { connect: async () => server }, addEventListener: () => {} }) } },
    });

    const client = new BleProvisioningClient();
    await client.connect();
    await expect(client.calibrationStart('hx711_organic', {
      reference_mass_mg: 2000000,
      tare_counts: 12,
    })).resolves.toMatchObject({ operation: 'START', status: 'ACCEPTED' });
  });
});
