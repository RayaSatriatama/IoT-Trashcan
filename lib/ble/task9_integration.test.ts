import { afterEach, describe, expect, it } from 'vitest';

import {
  assembleBleFrames,
  decodeBleFrame,
  encodeBleFrames,
  isWebBluetoothAvailable,
} from './protocol';
import { BleProvisioningClient } from './client';

type Listener = (event: { target?: { value?: DataView } }) => void;
type DisconnectListener = () => void;

class FakeCharacteristic {
  readonly writes: Uint8Array[] = [];
  private readonly listeners: Listener[] = [];
  private pendingRequest: Uint8Array[] = [];
  private readonly onCommand: (command: string, requestId: string, emit: (text: string) => void) => void;
  private events: FakeCharacteristic | null = null;

  constructor(onCommand: (command: string, requestId: string, emit: (text: string) => void) => void) {
    this.onCommand = onCommand;
  }

  setEventSink(events: FakeCharacteristic): void { this.events = events; }

  async writeValue(value: BufferSource): Promise<void> {
    const bytes = value instanceof ArrayBuffer
      ? new Uint8Array(value)
      : new Uint8Array(value.buffer, value.byteOffset, value.byteLength);
    const copy = bytes.slice();
    this.writes.push(copy);
    this.pendingRequest.push(copy);
    const frame = decodeBleFrame(copy);
    if (this.pendingRequest.length !== frame.chunkCount) return;
    const request = this.pendingRequest;
    this.pendingRequest = [];
    const parsed = request.map(decodeBleFrame);
    const command = new TextDecoder().decode(assembleBleFrames(parsed, 'F1')).trim();
    this.onCommand(command, frame.requestId, (text) => {
      if (!this.events) throw new Error('BLE event sink is not configured');
      encodeBleFrames(frame.requestId, new TextEncoder().encode(text), 'R1')
        .forEach((responseFrame) => this.events?.emit(responseFrame));
    });
  }

  async startNotifications(): Promise<FakeCharacteristic> { return this; }

  addEventListener(_event: string, listener: Listener): void { this.listeners.push(listener); }

  emit(frame: Uint8Array): void {
    const value = new DataView(frame.buffer, frame.byteOffset, frame.byteLength);
    this.listeners.forEach((listener) => listener({ target: { value } }));
  }
}

class FakeDevice {
  readonly name = 'SIGAP test device';
  private readonly listeners: DisconnectListener[] = [];
  readonly gatt = { connect: async () => this.server };

  constructor(private readonly server: FakeServer) {}

  addEventListener(_event: string, listener: DisconnectListener): void { this.listeners.push(listener); }

  disconnect(): void { this.listeners.forEach((listener) => listener()); }
}

class FakeServer {
  constructor(readonly control: FakeCharacteristic, readonly events: FakeCharacteristic) {}

  async getPrimaryService(_uuid: string): Promise<FakeServer> { return this; }

  async getCharacteristic(uuid: string): Promise<FakeCharacteristic> {
    return uuid.includes('0002-') ? this.control : this.events;
  }
}

const originalNavigator = Object.getOwnPropertyDescriptor(globalThis, 'navigator');
const originalSecureContext = Object.getOwnPropertyDescriptor(globalThis, 'isSecureContext');
const originalLocalStorage = Object.getOwnPropertyDescriptor(globalThis, 'localStorage');
const originalFetch = Object.getOwnPropertyDescriptor(globalThis, 'fetch');

function installBluetooth(
  onCommand: (command: string, requestId: string, emit: (text: string) => void) => void,
  onRequest: () => void,
): { control: FakeCharacteristic; device: FakeDevice } {
  const control = new FakeCharacteristic(onCommand);
  const events = new FakeCharacteristic(() => {});
  control.setEventSink(events);
  const server = new FakeServer(control, events);
  const device = new FakeDevice(server);
  Object.defineProperty(globalThis, 'isSecureContext', { configurable: true, value: true });
  Object.defineProperty(globalThis, 'navigator', {
    configurable: true,
    value: {
      bluetooth: {
        requestDevice: async () => {
          onRequest();
          return device;
        },
      },
    },
  });
  return { control, device };
}

function writtenCommands(writes: readonly Uint8Array[]): string[] {
  const grouped = new Map<string, ReturnType<typeof decodeBleFrame>[]>();
  writes.forEach((bytes) => {
    const frame = decodeBleFrame(bytes);
    const frames = grouped.get(frame.requestId) ?? [];
    frames.push(frame);
    grouped.set(frame.requestId, frames);
  });
  return [...grouped.values()]
    .map((frames) => new TextDecoder().decode(assembleBleFrames(frames, 'F1')).trim());
}

afterEach(() => {
  if (originalNavigator) Object.defineProperty(globalThis, 'navigator', originalNavigator);
  else Reflect.deleteProperty(globalThis, 'navigator');
  if (originalSecureContext) Object.defineProperty(globalThis, 'isSecureContext', originalSecureContext);
  else Reflect.deleteProperty(globalThis, 'isSecureContext');
  if (originalLocalStorage) Object.defineProperty(globalThis, 'localStorage', originalLocalStorage);
  else Reflect.deleteProperty(globalThis, 'localStorage');
  if (originalFetch) Object.defineProperty(globalThis, 'fetch', originalFetch);
  else Reflect.deleteProperty(globalThis, 'fetch');
});

describe('Task 9 Web Bluetooth provisioning boundary', () => {
  it('gates unsupported and insecure browser contexts before device selection', async () => {
    Object.defineProperty(globalThis, 'isSecureContext', { configurable: true, value: false });
    Object.defineProperty(globalThis, 'navigator', { configurable: true, value: { bluetooth: { requestDevice: async () => { throw new Error('must not be called'); } } } });
    expect(isWebBluetoothAvailable()).toBe(false);
    await expect(new BleProvisioningClient().connect()).rejects.toThrow('HTTPS');

    Object.defineProperty(globalThis, 'isSecureContext', { configurable: true, value: true });
    Object.defineProperty(globalThis, 'navigator', { configurable: true, value: {} });
    expect(isWebBluetoothAvailable()).toBe(false);
    await expect(new BleProvisioningClient().connect()).rejects.toThrow('HTTPS');
  });

  it('connects only after the user-triggered client call and requests the SIGAP service', async () => {
    let requestCount = 0;
    let requestedService = '';
    const harness = installBluetooth((command, requestId, emit) => {
      expect(command).toBe('GET_STATUS');
      emit('OK STATUS state=CONNECTED ssid=TestNetwork');
      expect(requestId).toMatch(/^\d+$/u);
    }, () => { requestCount += 1; });
    Object.defineProperty(globalThis, 'navigator', {
      configurable: true,
      value: {
        bluetooth: {
          requestDevice: async (options: { filters: Array<{ services: string[] }> }) => {
            requestedService = options.filters[0].services[0];
            requestCount += 1;
            return harness.device;
          },
        },
      },
    });
    expect(requestCount).toBe(0);
    const client = new BleProvisioningClient();
    await client.connect();
    expect(requestCount).toBe(1);
    expect(requestedService).toBe('7d3f0001-7d3f-4a53-9a4b-534947415000');
    await expect(client.status()).resolves.toEqual({ state: 'CONNECTED', ssid: 'TestNetwork' });
  });

  it('scans, retries saved WiFi, and reports status while connected', async () => {
    let scanCalls = 0;
    let retryCalls = 0;
    const harness = installBluetooth((command, _requestId, emit) => {
      if (command === 'SCAN_WIFI') {
        scanCalls += 1;
        emit('OK SCAN PENDING');
        emit('OK SCAN READY count=2\nTestNetwork,-38,1\nGuest,-72,0');
      } else if (command === 'RETRY_WIFI') {
        retryCalls += 1;
        emit('OK RETRY_WIFI');
      } else if (command === 'GET_STATUS') {
        emit('OK STATUS state=CONNECTED ssid=TestNetwork');
      } else {
        throw new Error(`unexpected command ${command}`);
      }
    }, () => {});
    const client = new BleProvisioningClient({ requestTimeoutMs: 100 });
    await client.connect();
    await expect(client.scan()).resolves.toEqual([
      { ssid: 'TestNetwork', rssi: -38, secure: true },
      { ssid: 'Guest', rssi: -72, secure: false },
    ]);
    expect(scanCalls).toBe(1);
    await expect(client.retryWifi()).resolves.toEqual({ state: 'CONNECTING', ssid: null });
    expect(retryCalls).toBe(1);
    await expect(client.status()).resolves.toEqual({ state: 'CONNECTED', ssid: 'TestNetwork' });
    harness.device.disconnect();
    await expect(client.status()).rejects.toThrow('Hubungkan perangkat');
  });

  it('sends credentials only over BLE and never through browser storage or fetch', async () => {
    const password = 'synthetic-pass-1234';
    let bleCommand = '';
    let storageWrites = 0;
    let fetchCalls = 0;
    Object.defineProperty(globalThis, 'localStorage', {
      configurable: true,
      value: { setItem: () => { storageWrites += 1; }, getItem: () => null, removeItem: () => {} },
    });
    Object.defineProperty(globalThis, 'fetch', { configurable: true, value: async () => { fetchCalls += 1; throw new Error('network must not be used'); } });
    const harness = installBluetooth((command, _requestId, emit) => {
      bleCommand = command;
      if (command.startsWith('CONNECT_WIFI ')) emit('OK CONNECTING');
      else emit('OK STATUS state=CONNECTED ssid=TestNetwork');
    }, () => {});
    const client = new BleProvisioningClient({ connectTimeoutMs: 100, connectPollIntervalMs: 0, sleep: async () => {} });
    await client.connect();
    await expect(client.connectWifi('TestNetwork', password)).resolves.toEqual({ state: 'CONNECTED', ssid: 'TestNetwork' });
    expect(bleCommand).toBe('GET_STATUS');
    expect(harness.control.writes.length).toBeGreaterThan(1);
    expect(writtenCommands(harness.control.writes)).toContain(`CONNECT_WIFI TestNetwork ${password}`);
    expect(storageWrites).toBe(0);
    expect(fetchCalls).toBe(0);
  });

  it('recovers after a GATT disconnect and allows a fresh user-triggered connection', async () => {
    let requestCount = 0;
    const harness = installBluetooth((command, _requestId, emit) => {
      if (command === 'GET_STATUS') emit('OK STATUS state=CONNECTED ssid=TestNetwork');
      else emit('OK PONG');
    }, () => { requestCount += 1; });
    const client = new BleProvisioningClient();
    await client.connect();
    harness.device.disconnect();
    await expect(client.status()).rejects.toThrow('Hubungkan perangkat');
    await client.connect();
    expect(requestCount).toBe(2);
    await expect(client.status()).resolves.toEqual({ state: 'CONNECTED', ssid: 'TestNetwork' });
  });
});
