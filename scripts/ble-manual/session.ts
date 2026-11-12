import {
  assembleBleFrames, decodeBleFrame, decodeBleResponse, encodeBleFrames, isBleChunkAcknowledgement,
  SIGAP_BLE_CONTROL_UUID, SIGAP_BLE_EVENTS_UUID, SIGAP_BLE_SERVICE_UUID,
} from '../../lib/ble/protocol';

// Arduino ESP.getEfuseMac() raw uint64 on the little-endian ESP32-S3 target.
export const EXPECTED_DEVICE_ID = '000050c1f516a398';
export const DIAGNOSTIC_STATUS = 'OK STATUS mode=BLE_DIAGNOSTIC wifi=DISABLED';
export type ManualCharacteristic = {
  writeValue: (value: BufferSource) => Promise<void>;
  startNotifications: () => Promise<unknown>;
  addEventListener: (type: string, listener: (event: { target?: { value?: DataView } }) => void) => void;
};
export type ManualServer = {
  getPrimaryService: (uuid: string) => Promise<{ getCharacteristic: (uuid: string) => Promise<ManualCharacteristic> }>;
  disconnect: () => void;
};
export type ManualDevice = {
  name?: string;
  gatt?: { connect: () => Promise<ManualServer> };
  addEventListener: (type: string, listener: () => void) => void;
};
export type ManualState = { connected: boolean; name: string | null; verifiedId: string | null; status: string | null };
type RequestOptions = { filters: { namePrefix: string }[]; optionalServices: string[] };
type Options = { requestDevice?: (options: RequestOptions) => Promise<ManualDevice>; onChange?: (state: ManualState) => void };
type Waiter = {
  id: string; frames: ReturnType<typeof decodeBleFrame>[];
  resolve: (body: string) => void; reject: (error: Error) => void;
  timer: ReturnType<typeof setTimeout>;
};

export class ManualBleSession {
  private snapshot: ManualState = { connected: false, name: null, verifiedId: null, status: null };
  private selectedDevice: ManualDevice | null = null;
  private server: ManualServer | null = null;
  private control: ManualCharacteristic | null = null;
  private pending: Waiter | null = null;
  private nextId = 1;
  private generation = 0;
  private readonly requestDevice: NonNullable<Options['requestDevice']>;
  private readonly onChange?: Options['onChange'];

  constructor(options: Options = {}) {
    this.onChange = options.onChange;
    this.requestDevice = options.requestDevice ?? ((request) => {
      const adapter = (globalThis.navigator as unknown as {
        bluetooth?: { requestDevice: NonNullable<Options['requestDevice']> };
      })?.bluetooth;
      if (!globalThis.isSecureContext || !adapter) throw new Error('UNSUPPORTED_BROWSER');
      return adapter.requestDevice(request);
    });
  }

  get state(): ManualState { return { ...this.snapshot }; }

  private update(change: Partial<ManualState>): void {
    this.snapshot = { ...this.snapshot, ...change };
    this.onChange?.(this.state);
  }

  private clear(): void {
    this.generation += 1;
    this.pending?.reject(new Error('DISCONNECTED'));
    this.server = null;
    this.control = null;
    this.update({ connected: false, verifiedId: null, status: null });
  }

  async choose(): Promise<void> {
    this.disconnect();
    this.selectedDevice = null;
    this.update({ name: null });
    const device = await this.requestDevice({
      filters: [{ namePrefix: 'SIGAP-' }], optionalServices: [SIGAP_BLE_SERVICE_UUID],
    });
    this.selectedDevice = device;
    this.update({ name: device.name ?? null });
    device.addEventListener('gattserverdisconnected', () => {
      if (this.selectedDevice === device) this.clear();
    });
    await this.open(device);
  }

  private async open(device: ManualDevice): Promise<void> {
    const generation = ++this.generation;
    const ensureCurrent = () => {
      if (generation !== this.generation) throw new Error('CANCELLED');
    };
    this.update({ connected: false, verifiedId: null, status: null });
    if (!device.gatt) throw new Error('GATT_UNAVAILABLE');
    try {
      const server = await device.gatt.connect();
      if (generation !== this.generation) {
        server.disconnect();
        throw new Error('CANCELLED');
      }
      this.server = server;
      const service = await this.server.getPrimaryService(SIGAP_BLE_SERVICE_UUID);
      ensureCurrent();
      this.control = await service.getCharacteristic(SIGAP_BLE_CONTROL_UUID);
      ensureCurrent();
      const events = await service.getCharacteristic(SIGAP_BLE_EVENTS_UUID);
      ensureCurrent();
      events.addEventListener('characteristicvaluechanged', (event) => {
        const value = event.target?.value;
        const waiter = this.pending;
        if (!value || !waiter) return;
        try {
          const frame = decodeBleFrame(new Uint8Array(value.buffer, value.byteOffset, value.byteLength));
          if (frame.version !== 'R1' || frame.requestId !== waiter.id || isBleChunkAcknowledgement(frame)) return;
          if (waiter.frames.some((previous) => previous.chunkIndex === frame.chunkIndex)) return;
          waiter.frames.push(frame);
          if (waiter.frames.length === frame.chunkCount) {
            waiter.resolve(new TextDecoder('utf-8', { fatal: true }).decode(assembleBleFrames(waiter.frames)));
          }
        } catch {
          // Malformed responses never become results; the bounded request expires.
        }
      });
      await events.startNotifications();
      ensureCurrent();
      this.update({ connected: true });
    } catch (error) {
      if (generation === this.generation) this.disconnect();
      throw error;
    }
  }

  async reconnect(): Promise<void> {
    const device = this.selectedDevice;
    if (!device) throw new Error('SELECT_DEVICE_FIRST');
    this.disconnect();
    await this.open(device);
  }

  disconnect(): void {
    const server = this.server;
    this.clear();
    server?.disconnect();
  }

  async verifyIdentity(): Promise<string> {
    const generation = this.generation;
    const body = await this.send('GET_DEVICE_ID');
    if (generation !== this.generation) throw new Error('DISCONNECTED');
    const response = decodeBleResponse(new TextEncoder().encode(body));
    if (!response.ok || response.deviceId !== EXPECTED_DEVICE_ID) {
      this.disconnect();
      throw new Error('IDENTITY_MISMATCH');
    }
    this.update({ verifiedId: response.deviceId });
    return response.deviceId;
  }

  async readStatus(): Promise<string> {
    if (this.snapshot.verifiedId !== EXPECTED_DEVICE_ID) throw new Error('IDENTITY_REQUIRED');
    const generation = this.generation;
    const body = await this.send('GET_STATUS');
    if (generation !== this.generation) throw new Error('DISCONNECTED');
    if (body !== DIAGNOSTIC_STATUS) throw new Error('UNEXPECTED_DIAGNOSTIC_STATUS');
    this.update({ status: body });
    return body;
  }

  private async send(command: 'GET_DEVICE_ID' | 'GET_STATUS'): Promise<string> {
    if (command !== 'GET_DEVICE_ID' && command !== 'GET_STATUS') throw new Error('COMMAND_NOT_ALLOWED');
    const control = this.control;
    if (!control || !this.snapshot.connected) throw new Error('NOT_CONNECTED');
    if (this.pending) throw new Error('REQUEST_IN_PROGRESS');
    if (this.nextId > 9999999999) throw new Error('REQUEST_LIMIT');
    const id = String(this.nextId++);
    return new Promise((resolve, reject) => {
      const waiter: Waiter = {
        id, frames: [],
        resolve: (body) => { clearTimeout(waiter.timer); if (this.pending === waiter) this.pending = null; resolve(body); },
        reject: (error) => { clearTimeout(waiter.timer); if (this.pending === waiter) this.pending = null; reject(error); },
        timer: setTimeout(() => waiter.reject(new Error('RESPONSE_TIMEOUT')), 5000),
      };
      this.pending = waiter;
      void (async () => {
        for (const frame of encodeBleFrames(id, new TextEncoder().encode(command))) {
          if (this.pending !== waiter) return;
          await control.writeValue(frame as unknown as BufferSource);
        }
      })().catch(() => waiter.reject(new Error('WRITE_FAILED')));
    });
  }
}
