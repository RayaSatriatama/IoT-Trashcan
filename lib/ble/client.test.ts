import { afterEach, describe, expect, it, vi } from 'vitest';

import {
  assembleBleFrames,
  decodeBleFrame,
  encodeBleFrames,
  SIGAP_BLE_SERVICE_UUID,
  type BleFrame,
} from './protocol';
import { BleProvisioningClient } from './client';

type Listener = (event: { target?: { value?: DataView } }) => void;

class FakeCharacteristic {
  readonly writes: Uint8Array[] = [];
  private readonly listeners: Listener[] = [];
  private readonly onRequest: (frames: Uint8Array[]) => void;
  private onProgress?: (frame: Uint8Array) => void;
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
    if (parsed.chunkIndex < parsed.chunkCount - 1) {
      const acknowledgement = encodeBleFrames(parsed.requestId, new TextEncoder().encode('OK CHUNK'), 'R1')[0];
      const wrongRequestId = String((Number(parsed.requestId) + 1) % 0x1_0000_0000);
      this.onProgress?.(encodeBleFrames(wrongRequestId, new TextEncoder().encode('OK CHUNK'), 'R1')[0]);
      this.onProgress?.(acknowledgement);
      this.onProgress?.(acknowledgement);
    }
    if (this.requestFrames.length === parsed.chunkCount) {
      const complete = this.requestFrames;
      this.requestFrames = [];
      this.onRequest(complete);
    }
  }

  async startNotifications(): Promise<FakeCharacteristic> { return this; }

  addEventListener(_event: string, listener: Listener): void { this.listeners.push(listener); }

  setProgressHandler(handler: (frame: Uint8Array) => void): void { this.onProgress = handler; }

  emit(frame: Uint8Array): void {
    const value = new DataView(frame.buffer, frame.byteOffset, frame.byteLength);
    this.listeners.forEach((listener) => listener({ target: { value } }));
  }
}

class FailingNotificationCharacteristic extends FakeCharacteristic {
  override async startNotifications(): Promise<FakeCharacteristic> {
    throw new Error('NOTIFY_FAILED');
  }
}

class FakeServer {
  requestedServiceUuid: string | null = null;

  constructor(private readonly control: FakeCharacteristic, private readonly events: FakeCharacteristic) {
    this.control.setProgressHandler((frame) => this.events.emit(frame));
  }
  async getPrimaryService(uuid: string): Promise<{ getCharacteristic: (uuid: string) => Promise<FakeCharacteristic> }> {
    this.requestedServiceUuid = uuid;
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
  it('uses a name-prefix discovery filter and requests the SIGAP service optionally', async () => {
    const control = new FakeCharacteristic();
    const events = new FakeCharacteristic();
    const server = new FakeServer(control, events);
    let requestedOptions: unknown;
    Object.defineProperty(globalThis, 'isSecureContext', { configurable: true, value: true });
    Object.defineProperty(globalThis, 'navigator', {
      configurable: true,
      value: {
        bluetooth: {
          requestDevice: async (options: unknown) => {
            requestedOptions = options;
            return { gatt: { connect: async () => server }, addEventListener: () => {} };
          },
        },
      },
    });

    await new BleProvisioningClient().connect();

    expect(requestedOptions).toEqual({
      filters: [{ namePrefix: 'SIGAP-' }],
      optionalServices: [SIGAP_BLE_SERVICE_UUID],
    });
    expect(server.requestedServiceUuid).toBe(SIGAP_BLE_SERVICE_UUID);
  });

  it('does not commit a chooser result after the connection was closed', async () => {
    let resolveChooser!: (device: { gatt: { connect: () => Promise<FakeServer> }; name: string; addEventListener: () => void }) => void;
    const control = new FakeCharacteristic();
    const events = new FakeCharacteristic();
    const server = new FakeServer(control, events);
    const identity = vi.fn();
    const connection = vi.fn();
    Object.defineProperty(globalThis, 'isSecureContext', { configurable: true, value: true });
    Object.defineProperty(globalThis, 'navigator', {
      configurable: true,
      value: { bluetooth: { requestDevice: () => new Promise((resolve) => { resolveChooser = resolve; }) } },
    });
    const client = new BleProvisioningClient({ onDeviceIdentityChange: identity, onConnectionChange: connection });
    const connecting = client.connect();
    client.close();
    resolveChooser({ gatt: { connect: async () => server }, name: 'SIGAP-old', addEventListener: () => {} });
    await expect(connecting).rejects.toThrow('BLE terputus');
    expect(client.verifiedDeviceId).toBeNull();
    expect(identity).not.toHaveBeenCalledWith('SIGAP-old');
    expect(connection).not.toHaveBeenCalledWith(true);
  });

  it('connects over ordinary GATT before checking the routed device identity', async () => {
    let control: FakeCharacteristic;
    let events: FakeCharacteristic;
    control = new FakeCharacteristic((requestFrames) => {
      const first = decodeBleFrame(requestFrames[0]);
      encodeBleFrames(first.requestId, new TextEncoder().encode(
        'OK DEVICE_ID id=0000000000000001',
      ), 'R1').forEach((frame) => events.emit(frame));
    });
    events = new FakeCharacteristic();
    const server = new FakeServer(control, events);
    Object.defineProperty(globalThis, 'isSecureContext', { configurable: true, value: true });
    Object.defineProperty(globalThis, 'navigator', {
      configurable: true,
      value: { bluetooth: { requestDevice: async () => ({
        gatt: { connect: async () => server },
        addEventListener: () => {},
      }) } },
    });

    const identityChange = vi.fn();
    const client = new BleProvisioningClient({
      requestTimeoutMs: 25,
      onVerifiedDeviceIdentityChange: identityChange,
    });
    await client.connect();
    expect(client.verifiedDeviceId).toBeNull();
    expect(control.writes).toEqual([]);
    await expect(client.verifyDeviceIdentity('0000000000000001'))
      .resolves.toBe('0000000000000001');
    expect(control.writes).toHaveLength(2);
    expect(identityChange).toHaveBeenNthCalledWith(1, null);
    expect(identityChange).toHaveBeenNthCalledWith(2, '0000000000000001');
  });

  it('frames GET_STATUS at request ID 10 without a newline-only trailing frame', async () => {
    let control: FakeCharacteristic;
    let events: FakeCharacteristic;
    control = new FakeCharacteristic((requestFrames) => {
      const first = decodeBleFrame(requestFrames[0]);
      const command = new TextDecoder().decode(assembleBleFrames(requestFrames.map(decodeBleFrame), 'F1'));
      const response = command.trim() === 'GET_DEVICE_ID'
        ? 'OK DEVICE_ID id=0000000000000001' : 'OK STATUS state=CONNECTED ssid=';
      encodeBleFrames(first.requestId, new TextEncoder().encode(response), 'R1').forEach((frame) => events.emit(frame));
    });
    events = new FakeCharacteristic();
    const server = new FakeServer(control, events);
    Object.defineProperty(globalThis, 'isSecureContext', { configurable: true, value: true });
    Object.defineProperty(globalThis, 'navigator', { configurable: true,
      value: { bluetooth: { requestDevice: async () => ({ gatt: { connect: async () => server }, addEventListener: () => {} }) } } });
    const client = new BleProvisioningClient({ requestTimeoutMs: 25 });
    await client.connect();
    await client.verifyDeviceIdentity('0000000000000001');
    Object.defineProperty(client, 'nextRequestId', { configurable: true, value: 10, writable: true });
    await expect(client.status()).resolves.toEqual({ state: 'CONNECTED', ssid: null });
    const frames = control.writes.map(decodeBleFrame).filter((frame) => frame.requestId === '10');
    expect(frames.length).toBeGreaterThan(0);
    expect(frames.every((frame) => frame.payload.length > 0)).toBe(true);
    expect(new TextDecoder().decode(assembleBleFrames(frames, 'F1'))).toBe('GET_STATUS');
  });

  it('writes framed chunks and waits for persisted CONNECTED status', async () => {
    let statusRequests = 0;
    let control: FakeCharacteristic;
    let events: FakeCharacteristic;
    control = new FakeCharacteristic((requestFrames) => {
      const first = decodeBleFrame(requestFrames[0]);
      const command = new TextDecoder().decode(assembleBleFrames(requestFrames.map(decodeBleFrame), 'F1')).trim();
      let response = 'OK PONG';
      if (command === 'GET_DEVICE_ID') response = 'OK DEVICE_ID id=0000000000000001';
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

    const client = new BleProvisioningClient({ requestTimeoutMs: 25, connectPollIntervalMs: 0, sleep: async () => {} });
    await client.connect();
    await client.verifyDeviceIdentity('0000000000000001');
    Object.defineProperty(client, 'nextRequestId', { configurable: true, value: 1000, writable: true });
    const status = await client.connectWifi('TestNetwork', 'test-password');

    expect(status).toEqual({ state: 'CONNECTED', ssid: 'TestNetwork' });
    expect(control.writes.length).toBeGreaterThan(1);
    expect(control.writes.every((frame) => frame.byteLength <= 20)).toBe(true);
    const requestIds = new Set(control.writes.map((frame) => decodeBleFrame(frame).requestId));
    expect(requestIds.size).toBe(4);
  });

  it('does not treat CONNECTING as a successful provisioning result', async () => {
    let control: FakeCharacteristic;
    let events: FakeCharacteristic;
    control = new FakeCharacteristic((requestFrames) => {
      const first = decodeBleFrame(requestFrames[0]);
      const command = new TextDecoder().decode(assembleBleFrames(requestFrames.map(decodeBleFrame), 'F1')).trim();
      const response = command === 'GET_DEVICE_ID'
        ? 'OK DEVICE_ID id=0000000000000001'
        : command.startsWith('CONNECT_WIFI ')
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
    await client.verifyDeviceIdentity('0000000000000001');
    await expect(client.connectWifi('TestNetwork', 'test-password')).rejects.toThrow('Koneksi Wi-Fi tidak selesai');
  });

  it('rejects a GATT session when notification setup fails', async () => {
    const control = new FakeCharacteristic();
    const events = new FailingNotificationCharacteristic();
    const server = new FakeServer(control, events);
    Object.defineProperty(globalThis, 'isSecureContext', { configurable: true, value: true });
    Object.defineProperty(globalThis, 'navigator', {
      configurable: true,
      value: { bluetooth: { requestDevice: async () => ({ gatt: { connect: async () => server }, addEventListener: () => {} }) } },
    });

    const client = new BleProvisioningClient();
    await expect(client.connect()).rejects.toThrow('NOTIFY_FAILED');
    expect(client.verifiedDeviceId).toBeNull();
    client.close();
  });

  it('returns raw row count on the real framed client path while legacy scan remains networks-only', async () => {
    let control: FakeCharacteristic;
    let events: FakeCharacteristic;
    control = new FakeCharacteristic((requestFrames) => {
      const first = decodeBleFrame(requestFrames[0]);
      const command = new TextDecoder().decode(assembleBleFrames(requestFrames.map(decodeBleFrame), 'F1')).trim();
      const response = command === 'GET_DEVICE_ID' ? 'OK DEVICE_ID id=0000000000000001'
        : command === 'SCAN_WIFI' ? `OK SCAN READY count=2${String.fromCharCode(10)}Lab,-40,1${String.fromCharCode(10)},-90,0` : 'ERR unexpected';
      encodeBleFrames(first.requestId, new TextEncoder().encode(response), 'R1').forEach((frame) => events.emit(frame));
    });
    events = new FakeCharacteristic(); const server = new FakeServer(control, events);
    Object.defineProperty(globalThis, 'isSecureContext', { configurable: true, value: true });
    Object.defineProperty(globalThis, 'navigator', { configurable: true,
      value: { bluetooth: { requestDevice: async () => ({ gatt: { connect: async () => server }, addEventListener: () => {} }) } } });
    const client = new BleProvisioningClient({ requestTimeoutMs: 100 }); await client.connect();
    await client.verifyDeviceIdentity('0000000000000001');
    await expect(client.scanDetailed()).resolves.toEqual({ networks: [{ ssid: 'Lab', rssi: -40, secure: true }], rawCount: 2 });
    await expect(client.scan()).resolves.toEqual([{ ssid: 'Lab', rssi: -40, secure: true }]);
    expect(new Set(control.writes.map(decodeBleFrame).filter((frame) => frame.requestId !== '1').map((frame) => frame.requestId)).size).toBe(2);
  });

  it('reassembles an asynchronous scan response after the pending acknowledgement', async () => {
    let control: FakeCharacteristic;
    let events: FakeCharacteristic;
    control = new FakeCharacteristic((requestFrames) => {
      const first = decodeBleFrame(requestFrames[0]);
      const command = new TextDecoder().decode(assembleBleFrames(requestFrames.map(decodeBleFrame), 'F1')).trim();
      if (command === 'GET_DEVICE_ID') {
        encodeBleFrames(first.requestId, new TextEncoder().encode('OK DEVICE_ID id=0000000000000001'), 'R1').forEach((frame) => events.emit(frame));
        return;
      }
      if (command !== 'SCAN_WIFI') return;
      encodeBleFrames(first.requestId, new TextEncoder().encode('OK SCAN PENDING'), 'R1').forEach((frame) => events.emit(frame));
      encodeBleFrames(first.requestId, new TextEncoder().encode('OK SCAN READY count=2\nTestNetwork,-39,1\n,-88,0'), 'R1').forEach((frame) => events.emit(frame));
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
    await client.verifyDeviceIdentity('0000000000000001');
    await expect(client.scan()).resolves.toEqual([{ ssid: 'TestNetwork', rssi: -39, secure: true }]);
  });

  it('reports safe scan boundary events without changing the result when the observer throws', async () => {
    let control: FakeCharacteristic;
    let events: FakeCharacteristic;
    control = new FakeCharacteristic((requestFrames) => {
      const first = decodeBleFrame(requestFrames[0]);
      const command = new TextDecoder().decode(assembleBleFrames(requestFrames.map(decodeBleFrame), 'F1')).trim();
      if (command === 'GET_DEVICE_ID') {
        encodeBleFrames(first.requestId, new TextEncoder().encode('OK DEVICE_ID id=0000000000000001'), 'R1').forEach((frame) => events.emit(frame));
      } else {
        encodeBleFrames(first.requestId, new TextEncoder().encode('OK SCAN PENDING'), 'R1').forEach((frame) => events.emit(frame));
        encodeBleFrames(first.requestId, new TextEncoder().encode('OK SCAN READY count=1\nsynthetic-secret-ssid,-39,1'), 'R1').forEach((frame) => events.emit(frame));
      }
    });
    events = new FakeCharacteristic();
    const server = new FakeServer(control, events);
    Object.defineProperty(globalThis, 'isSecureContext', { configurable: true, value: true });
    Object.defineProperty(globalThis, 'navigator', {
      configurable: true,
      value: { bluetooth: { requestDevice: async () => ({ gatt: { connect: async () => server }, addEventListener: () => {} }) } },
    });

    const trace: unknown[] = [];
    const client = new BleProvisioningClient({ onDiagnosticEvent: (event) => { trace.push(event); throw new Error('synthetic-password'); } });
    await client.connect();
    await client.verifyDeviceIdentity('0000000000000001');
    await expect(client.scan()).resolves.toEqual([{ ssid: 'synthetic-secret-ssid', rssi: -39, secure: true }]);
    const serialized = JSON.stringify(trace);
    expect(serialized).toContain('scan_pending');
    expect(serialized).toContain('request_complete');
    expect(serialized).not.toContain('synthetic-secret-ssid');
    expect(serialized).not.toContain('synthetic-password');
  });

  it('reports a decoder rejection and the original request timeout without exposing response data', async () => {
    vi.useFakeTimers();
    try {
      let control: FakeCharacteristic;
      let events: FakeCharacteristic;
      control = new FakeCharacteristic((requestFrames) => {
        const first = decodeBleFrame(requestFrames[0]);
        const command = new TextDecoder().decode(assembleBleFrames(requestFrames.map(decodeBleFrame), 'F1')).trim();
        const response = command === 'GET_DEVICE_ID'
          ? 'OK DEVICE_ID id=0000000000000001'
          : 'OK SCAN READY count=1\n,-39,1 synthetic-password';
        encodeBleFrames(first.requestId, new TextEncoder().encode(response), 'R1').forEach((frame) => events.emit(frame));
      });
      events = new FakeCharacteristic();
      const server = new FakeServer(control, events);
      Object.defineProperty(globalThis, 'isSecureContext', { configurable: true, value: true });
      Object.defineProperty(globalThis, 'navigator', {
        configurable: true,
        value: { bluetooth: { requestDevice: async () => ({ gatt: { connect: async () => server }, addEventListener: () => {} }) } },
      });

      const trace: unknown[] = [];
      const client = new BleProvisioningClient({ requestTimeoutMs: 100, onDiagnosticEvent: (event) => trace.push(event) });
      await client.connect();
      await client.verifyDeviceIdentity('0000000000000001');
      const scanning = client.scan();
      const timedOut = expect(scanning).rejects.toThrow('Perangkat tidak merespons');
      await vi.advanceTimersByTimeAsync(100);
      await timedOut;
      const serialized = JSON.stringify(trace);
      expect(serialized).toContain('decoder_rejected');
      expect(serialized).toContain('scan_entry_invalid');
      expect(serialized).toContain('request_timeout');
      expect(serialized).not.toContain('synthetic-password');
      expect(control.writes).toHaveLength(3);
    } finally {
      vi.useRealTimers();
    }
  });

  it('does not retry a complete decoder-rejected scan when notification precedes write completion', async () => {
    vi.useFakeTimers();
    try {
      let control: FakeCharacteristic;
      let events: FakeCharacteristic;
      control = new FakeCharacteristic((requestFrames) => {
        const first = decodeBleFrame(requestFrames[0]);
        const command = new TextDecoder().decode(assembleBleFrames(requestFrames.map(decodeBleFrame), 'F1')).trim();
        const response = command === 'GET_DEVICE_ID'
          ? 'OK DEVICE_ID id=0000000000000001'
          : 'OK SCAN READY count=1\nsynthetic-network,-39,2';
        encodeBleFrames(first.requestId, new TextEncoder().encode(response), 'R1').forEach((frame) => events.emit(frame));
      });
      events = new FakeCharacteristic();
      const server = new FakeServer(control, events);
      Object.defineProperty(globalThis, 'isSecureContext', { configurable: true, value: true });
      Object.defineProperty(globalThis, 'navigator', { configurable: true,
        value: { bluetooth: { requestDevice: async () => ({ gatt: { connect: async () => server }, addEventListener: () => {} }) } } });
      const trace: { type?: string; requestId?: string; attempt?: number }[] = [];
      const client = new BleProvisioningClient({ requestTimeoutMs: 30_000, readOnlyMaxAttempts: 3,
        onDiagnosticEvent: (event) => trace.push(event) });
      await client.connect();
      await client.verifyDeviceIdentity('0000000000000001');
      const scanning = client.scan();
      const timedOut = expect(scanning).rejects.toThrow('Perangkat tidak merespons');
      await vi.runAllTimersAsync();
      await timedOut;
      const scanStarted = trace.find((event) => event.type === 'request_started' && event.attempt === 1 && event.requestId !== undefined && event.requestId !== '1');
      expect(trace.filter((event) => event.type === 'decoder_rejected')).toHaveLength(1);
      expect(control.writes.map(decodeBleFrame).filter((frame) => frame.requestId !== '1')).toHaveLength(1);
      expect(trace.filter((event) => event.type === 'request_started' && event.requestId === scanStarted?.requestId)).toHaveLength(1);
    } finally {
      vi.useRealTimers();
    }
  });

  it('limits read-only timeout retries to three fresh request IDs', async () => {
    vi.useFakeTimers();
    try {
      const control = new FakeCharacteristic();
      const events = new FakeCharacteristic();
      const server = new FakeServer(control, events);
      Object.defineProperty(globalThis, 'isSecureContext', { configurable: true, value: true });
      Object.defineProperty(globalThis, 'navigator', { configurable: true,
        value: { bluetooth: { requestDevice: async () => ({ gatt: { connect: async () => server }, addEventListener: () => {} }) } } });
      const trace: { type?: string; attempt?: number; maxAttempts?: number }[] = [];
      const client = new BleProvisioningClient({ requestTimeoutMs: 30_000, readOnlyMaxAttempts: 3,
        onDiagnosticEvent: (event) => trace.push(event) });
      await client.connect();
      const identity = client.verifyDeviceIdentity('0000000000000001');
      const timedOut = expect(identity).rejects.toThrow('Perangkat tidak merespons');
      await vi.runAllTimersAsync();
      await timedOut;
      const requestIds = [...new Set(control.writes.map(decodeBleFrame).map((frame) => frame.requestId))];
      expect(requestIds).toEqual(['1', '2', '3']);
      expect(trace.filter((event) => event.type === 'request_started').map(({ attempt, maxAttempts }) => [attempt, maxAttempts]))
        .toEqual([[1, 3], [2, 3], [3, 3]]);
    } finally {
      vi.useRealTimers();
    }
  });

  it('reports missing response fragments at timeout without accepting them', async () => {
    vi.useFakeTimers();
    try {
      let control: FakeCharacteristic;
      let events: FakeCharacteristic;
      control = new FakeCharacteristic((requestFrames) => {
        const first = decodeBleFrame(requestFrames[0]);
        const command = new TextDecoder().decode(assembleBleFrames(requestFrames.map(decodeBleFrame), 'F1')).trim();
        const response = command === 'GET_DEVICE_ID'
          ? 'OK DEVICE_ID id=0000000000000001'
          : 'x'.repeat(160);
        const frames = encodeBleFrames(first.requestId, new TextEncoder().encode(response), 'R1');
        (command === 'GET_DEVICE_ID' ? frames : frames.slice(0, 1)).forEach((frame) => events.emit(frame));
      });
      events = new FakeCharacteristic();
      const server = new FakeServer(control, events);
      Object.defineProperty(globalThis, 'isSecureContext', { configurable: true, value: true });
      Object.defineProperty(globalThis, 'navigator', {
        configurable: true,
        value: { bluetooth: { requestDevice: async () => ({ gatt: { connect: async () => server }, addEventListener: () => {} }) } },
      });

      const trace: unknown[] = [];
      const client = new BleProvisioningClient({ requestTimeoutMs: 100, onDiagnosticEvent: (event) => trace.push(event) });
      await client.connect();
      await client.verifyDeviceIdentity('0000000000000001');
      const scanning = client.scan();
      const timedOut = expect(scanning).rejects.toThrow('Perangkat tidak merespons');
      await vi.advanceTimersByTimeAsync(100);
      await timedOut;
      const timeout = trace.find((event) => (event as { type?: string }).type === 'request_timeout') as { receivedCount?: number; expectedCount?: number };
      expect(timeout).toMatchObject({ receivedCount: 1, expectedCount: 17 });
      expect(control.writes).toHaveLength(3);
    } finally {
      vi.useRealTimers();
    }
  });

  it('routes calibration commands through the bounded BLE session', async () => {
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

    const client = new BleProvisioningClient({ requestTimeoutMs: 25 });
    await client.connect();
    await expect(client.calibrationStatus('ds18b20_b3')).resolves.toEqual({
      version: 'CAL1', operation: 'STATUS', status: 'ACCEPTED', sessionId: '7',
      sensor: 'ds18b20_b3', data: { state: 'IDLE' },
    });
    expect(control.writes.every((frame) => frame.byteLength <= 20)).toBe(true);
    expect(control.writes.map(decodeBleFrame).every((frame) => frame.payload.length > 0 && !(frame.payload.length === 1 && frame.payload[0] === 10))).toBe(true);
  });

  it('rejects a concurrent request without replacing the pending response', async () => {
    let control: FakeCharacteristic;
    let events: FakeCharacteristic;
    let firstRequestId = '';
    control = new FakeCharacteristic((requestFrames) => {
      const first = decodeBleFrame(requestFrames[0]);
      const command = new TextDecoder().decode(assembleBleFrames(requestFrames.map(decodeBleFrame), 'F1')).trim();
      if (command === 'GET_DEVICE_ID') {
        encodeBleFrames(first.requestId, new TextEncoder().encode('OK DEVICE_ID id=0000000000000001'), 'R1').forEach((frame) => events.emit(frame));
        return;
      }
      if (command === 'GET_STATUS' && !firstRequestId) firstRequestId = first.requestId;
    });
    events = new FakeCharacteristic();
    const server = new FakeServer(control, events);
    Object.defineProperty(globalThis, 'isSecureContext', { configurable: true, value: true });
    Object.defineProperty(globalThis, 'navigator', {
      configurable: true,
      value: { bluetooth: { requestDevice: async () => ({ gatt: { connect: async () => server }, addEventListener: () => {} }) } },
    });

    const client = new BleProvisioningClient({ requestTimeoutMs: 100 });
    await client.connect();
    await client.verifyDeviceIdentity('0000000000000001');
    const first = client.status();
    await Promise.resolve();

    await expect(client.status()).rejects.toThrow('Permintaan BLE sebelumnya masih berjalan');
    expect(firstRequestId).toMatch(/^\d+$/u);
    encodeBleFrames(firstRequestId, new TextEncoder().encode('OK STATUS state=CONNECTED ssid=TestNetwork'), 'R1')
      .forEach((frame) => events.emit(frame));
    await expect(first).resolves.toEqual({ state: 'CONNECTED', ssid: 'TestNetwork' });
  });

  it('keeps the production request deadline fixed across duplicate progress acknowledgements', async () => {
    vi.useFakeTimers();
    try {
      const control = new FakeCharacteristic();
      const events = new FakeCharacteristic();
      const server = new FakeServer(control, events);
      Object.defineProperty(globalThis, 'isSecureContext', { configurable: true, value: true });
      Object.defineProperty(globalThis, 'navigator', {
        configurable: true,
        value: { bluetooth: { requestDevice: async () => ({ gatt: { connect: async () => server }, addEventListener: () => {} }) } },
      });

      const client = new BleProvisioningClient({ requestTimeoutMs: 100 });
      await client.connect();
      const verification = client.verifyDeviceIdentity('0000000000000001');
      await Promise.resolve();
      await Promise.resolve();
      expect(control.writes).toHaveLength(2);
      const timedOut = expect(verification).rejects.toThrow('Perangkat tidak merespons');
      await vi.advanceTimersByTimeAsync(100);
      await timedOut;
      expect(control.writes).toHaveLength(2);
      expect(client.verifiedDeviceId).toBeNull();
    } finally {
      vi.useRealTimers();
    }
  });

  it('rejects a request when closed after progress acknowledgements but before its terminal response', async () => {
    const control = new FakeCharacteristic();
    const events = new FakeCharacteristic();
    const server = new FakeServer(control, events);
    Object.defineProperty(globalThis, 'isSecureContext', { configurable: true, value: true });
    Object.defineProperty(globalThis, 'navigator', {
      configurable: true,
      value: { bluetooth: { requestDevice: async () => ({ gatt: { connect: async () => server }, addEventListener: () => {} }) } },
    });

    const client = new BleProvisioningClient({ requestTimeoutMs: 1000 });
    await client.connect();
    const verification = client.verifyDeviceIdentity('0000000000000001');
    await Promise.resolve();
    await Promise.resolve();
    expect(control.writes).toHaveLength(2);
    client.close();
    await expect(verification).rejects.toThrow('BLE terputus');
    expect(client.verifiedDeviceId).toBeNull();
    expect(control.writes).toHaveLength(2);
  });

  it('delivers first-time calibration setup parameters with START', async () => {
    let control: FakeCharacteristic;
    let events: FakeCharacteristic;
    control = new FakeCharacteristic((requestFrames) => {
      const first = decodeBleFrame(requestFrames[0]);
      const command = new TextDecoder().decode(assembleBleFrames(requestFrames.map(decodeBleFrame), 'F1')).trim();
      const response = command === 'GET_DEVICE_ID'
        ? 'OK DEVICE_ID id=0000000000000001'
        : command.startsWith('CAL START hx711_organic reference_mass_mg=2000000 tare_counts=12')
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
    await client.verifyDeviceIdentity('0000000000000001');
    await expect(client.calibrationStart('hx711_organic', {
      reference_mass_mg: 2000000,
      tare_counts: 12,
    })).resolves.toMatchObject({ operation: 'START', status: 'ACCEPTED' });
  });

  it('checks the selected device identity before calibration mutation', async () => {
    let identity = '00000000000000aa';
    let control: FakeCharacteristic;
    let events: FakeCharacteristic;
    control = new FakeCharacteristic((requestFrames) => {
      const first = decodeBleFrame(requestFrames[0]);
      const command = new TextDecoder().decode(assembleBleFrames(requestFrames.map(decodeBleFrame), 'F1')).trim();
      const response = command === 'GET_DEVICE_ID'
        ? `OK DEVICE_ID id=${identity}`
        : 'CAL1|op=START|status=ACCEPTED|session=9|sensor=hx711_organic|data=state=CAPTURING';
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
    await expect(client.calibrationStart('hx711_organic', { reference_mass_mg: 2000 }))
      .rejects.toThrow('Identitas perangkat BLE belum diverifikasi');
    await expect(client.verifyDeviceIdentity('00000000000000ab'))
      .rejects.toThrow('tidak cocok');
    expect(client.verifiedDeviceId).toBeNull();
    await expect(client.verifyDeviceIdentity('00000000000000aa')).resolves.toBe('00000000000000aa');
    await expect(client.calibrationStart('hx711_organic', { reference_mass_mg: 2000 }))
      .resolves.toMatchObject({ operation: 'START', status: 'ACCEPTED' });
    client.close();
    expect(client.verifiedDeviceId).toBeNull();
    identity = '00000000000000bb';
  });
});
