import { afterEach, describe, expect, it } from 'vitest';

import {
  assembleBleFrames,
  decodeBleFrame,
  encodeBleFrames,
  isWebBluetoothAvailable,
} from './protocol';
import { BleProvisioningClient } from './client';
import { hasValidatedPreview, nextStepAfterOperation, type CalibrationWizardStep } from '../calibration/wizard';

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
  it('completes the synthetic calibration journey through the real BLE client and CAL1 codec', async () => {
    const sensor = 'mq4';
    const session = '42';
    const proposalHash = 'sha256:' + 'a'.repeat(64);
    const evidenceHash = 'sha256:' + 'b'.repeat(64);
    const commands: string[] = [];
    const harness = installBluetooth((command, _requestId, emit) => {
      commands.push(command);
      const tokens = command.split(/\s+/u);
      expect(tokens[0]).toBe('CAL');
      const operation = tokens[1];
      expect(tokens[2]).toBe(sensor);
      const respond = (data: string) => emit(`CAL1|op=${operation}|status=ACCEPTED|session=${session}|sensor=${sensor}|data=${data}`);
      switch (operation) {
        case 'CAPABILITIES':
          respond('references=reference_mv:mV;criteria=samples:count,max_span_counts:counts;engineering=reference_mv:mV,full_scale_counts:counts,divider_numerator:ratio,divider_denominator:ratio');
          break;
        case 'START':
          expect(command).toContain('reference_mv=500');
          expect(command).toContain('full_scale_counts=4095');
          respond('state=ACTIVE');
          break;
        case 'CRITERIA':
          expect(command).toContain('max_span_counts=20');
          expect(command).toContain('samples=3');
          respond('state=CRITERIA_SET');
          break;
        case 'CAPTURE':
          expect(command).toContain('reference');
          respond('state=CAPTURING;evidence_count=3;evidence_hash=' + evidenceHash);
          break;
        case 'VALIDATE':
          respond('state=VALIDATED;evidence_count=3;evidence_hash=' + evidenceHash);
          break;
        case 'PREVIEW':
          respond(`proposal_hash=${proposalHash};profile_version=7;evidence_count=3;evidence_hash=${evidenceHash};expires_in_ms=60000`);
          break;
        case 'COMMIT':
          expect(command).toContain('expected_profile_hash=' + proposalHash);
          expect(command).toContain('expected_profile_version=7');
          respond(`state=COMMITTED;profile_key=${sensor};profile_hash=${proposalHash};profile_version=7`);
          break;
        default:
          throw new Error(`unexpected calibration operation ${operation}`);
      }
    }, () => {});
    const client = new BleProvisioningClient({ requestTimeoutMs: 200 });
    await client.connect();

    const capabilities = await client.calibrationCapabilities(sensor);
    expect(capabilities.sensor).toBe(sensor);
    expect(capabilities.data.engineering).toContain('reference_mv:mV');
    const start = await client.calibrationStart(sensor, {
      reference_mv: 500, full_scale_counts: 4095, divider_numerator: 1, divider_denominator: 1,
    });
    expect(start.data.state).toBe('ACTIVE');
    const criteria = await client.calibrationCriteria(sensor, { samples: 3, max_span_counts: 20 });
    expect(criteria.data.state).toBe('CRITERIA_SET');
    const capture = await client.calibrationCapture(sensor, 'reference');
    expect(capture.data.evidence_hash).toBe(evidenceHash);
    const validated = await client.calibrationValidate(sensor);
    expect(validated.data.state).toBe('VALIDATED');
    const previewResponse = await client.calibrationPreview(sensor);
    const preview = {
      sensor_id: sensor as 'mq4', proposal_hash: previewResponse.data.proposal_hash,
      profile_version: previewResponse.data.profile_version, evidence_count: Number(previewResponse.data.evidence_count),
      evidence_hash: previewResponse.data.evidence_hash, expires_at: new Date(Date.now() + 60_000).toISOString(), parameters: {},
    };
    expect(hasValidatedPreview(preview, sensor as 'mq4')).toBe(true);
    const committed = await client.calibrationCommit(sensor, {
      expected_profile_hash: preview.proposal_hash, expected_profile_version: preview.profile_version,
    });
    expect(committed.data.state).toBe('COMMITTED');
    expect(committed.data.profile_hash).toBe(proposalHash);
    expect(commands).toHaveLength(7);

    let step: CalibrationWizardStep = 'diagnostics';
    step = nextStepAfterOperation('START', 'ACCEPTED', step);
    expect(step).toBe('criteria');
    step = nextStepAfterOperation('SET_CRITERIA', 'ACCEPTED', step);
    expect(step).toBe('capture');
    step = nextStepAfterOperation('CAPTURE', 'CAPTURING', step);
    expect(step).toBe('validate');
    step = nextStepAfterOperation('VALIDATE', 'VALIDATED', step);
    expect(step).toBe('preview');
    step = nextStepAfterOperation('PREVIEW', 'PREVIEW_READY', step);
    expect(step).toBe('commit');
    expect(nextStepAfterOperation('COMMIT', 'COMMITTED', step)).toBe('done');
  });

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
