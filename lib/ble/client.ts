import {
  assembleBleFrames,
  decodeBleFrame,
  decodeBleResponse,
  encodeBleFrames,
  encodeBleCommand,
  isWebBluetoothAvailable,
  SIGAP_BLE_CONTROL_UUID,
  SIGAP_BLE_EVENTS_UUID,
  SIGAP_BLE_SERVICE_UUID,
  type WifiNetwork,
  type WifiStatus,
} from './protocol';

type BleCharacteristic = {
  writeValue: (value: BufferSource) => Promise<void>;
  startNotifications: () => Promise<BleCharacteristic>;
  addEventListener: (event: string, listener: (event: { target?: { value?: DataView } }) => void) => void;
};
type BleService = { getCharacteristic: (uuid: string) => Promise<BleCharacteristic> };
type BleServer = { getPrimaryService: (uuid: string) => Promise<BleService>; disconnect?: () => void };
type BleDevice = { gatt?: { connect: () => Promise<BleServer> }; addEventListener: (event: string, listener: () => void) => void };
type BluetoothAdapter = { requestDevice: (options: { filters: Array<{ services: string[] }> }) => Promise<BleDevice> };

export type BleProvisioningClientOptions = {
  requestTimeoutMs?: number;
  connectTimeoutMs?: number;
  connectPollIntervalMs?: number;
  sleep?: (milliseconds: number) => Promise<void>;
  onConnectionChange?: (connected: boolean) => void;
};

const bluetooth = (): BluetoothAdapter => {
  const value = (globalThis as unknown as { navigator?: { bluetooth?: BluetoothAdapter } }).navigator?.bluetooth;
  if (!value || !isWebBluetoothAvailable()) throw new Error('Web Bluetooth membutuhkan HTTPS dan browser yang mendukungnya');
  return value;
};

export class BleProvisioningClient {
  private device: BleDevice | null = null;
  private server: BleServer | null = null;
  private control: BleCharacteristic | null = null;
  private pending: {
    requestId: string;
    frames: Array<ReturnType<typeof decodeBleFrame>>;
    resolve: (response: ReturnType<typeof decodeBleResponse>) => void;
    reject: (error: Error) => void;
    accept: (response: ReturnType<typeof decodeBleResponse>) => boolean;
  } | null = null;
  private nextRequestId = 1;
  private readonly requestTimeoutMs: number;
  private readonly connectTimeoutMs: number;
  private readonly connectPollIntervalMs: number;
  private readonly sleep: (milliseconds: number) => Promise<void>;
  private readonly onConnectionChange?: (connected: boolean) => void;

  constructor(options: BleProvisioningClientOptions = {}) {
    this.requestTimeoutMs = options.requestTimeoutMs ?? 5000;
    this.connectTimeoutMs = options.connectTimeoutMs ?? 12000;
    this.connectPollIntervalMs = options.connectPollIntervalMs ?? 500;
    this.sleep = options.sleep ?? ((milliseconds) => new Promise((resolve) => globalThis.setTimeout(resolve, milliseconds)));
    this.onConnectionChange = options.onConnectionChange;
  }

  async connect(): Promise<void> {
    const device = await bluetooth().requestDevice({ filters: [{ services: [SIGAP_BLE_SERVICE_UUID] }] });
    if (!device.gatt) throw new Error('Perangkat BLE tidak menyediakan GATT');
    const server = await device.gatt.connect();
    this.server = server;
    const service = await server.getPrimaryService(SIGAP_BLE_SERVICE_UUID);
    this.control = await service.getCharacteristic(SIGAP_BLE_CONTROL_UUID);
    const events = await service.getCharacteristic(SIGAP_BLE_EVENTS_UUID);
    await events.startNotifications();
    events.addEventListener('characteristicvaluechanged', (event) => {
      const value = event.target?.value;
      if (!value) return;
      try {
        const frame = decodeBleFrame(new Uint8Array(value.buffer, value.byteOffset, value.byteLength));
        const waiter = this.pending;
        if (!waiter || frame.requestId !== waiter.requestId || frame.version !== 'R1') return;
        if (waiter.frames.some((candidate) => candidate.chunkIndex === frame.chunkIndex)) return;
        waiter.frames.push(frame);
        if (waiter.frames.length !== frame.chunkCount) return;
        const response = decodeBleResponse(assembleBleFrames(waiter.frames));
        if (!waiter.accept(response)) {
          waiter.frames.length = 0;
          return;
        }
        this.pending = null;
        if (response.ok) waiter.resolve(response);
        else waiter.reject(new Error(response.error || 'Perangkat menolak permintaan'));
      } catch {
        // Invalid frames are ignored; the request timeout provides bounded recovery.
      }
    });
    this.device = device;
    this.onConnectionChange?.(true);
    device.addEventListener('gattserverdisconnected', () => this.close());
  }

  close(): void {
    this.pending?.reject(new Error('BLE terputus'));
    this.pending = null;
    this.control = null;
    this.server?.disconnect?.();
    this.server = null;
    this.device = null;
    this.onConnectionChange?.(false);
  }

  async status(): Promise<WifiStatus> {
    const response = await this.request('STATUS');
    if (!response.status) throw new Error('Status WiFi tidak tersedia');
    return response.status;
  }

  async scan(): Promise<WifiNetwork[]> {
    const response = await this.request('SCAN', {}, (value) => !value.scanPending);
    return response.networks ?? [];
  }

  async connectWifi(ssid: string, password: string): Promise<WifiStatus> {
    if (!ssid.trim() || ssid.length > 32 || password.length > 64) throw new Error('SSID atau password tidak valid');
    const response = await this.request('CONNECT', { ssid, password });
    if (!response.status) throw new Error('Status koneksi tidak tersedia');
    if (response.status.state === 'CONNECTED') return response.status;
    if (response.status.state === 'SETUP_REQUIRED' || response.status.state === 'UNCONFIGURED') {
      throw new Error('Perangkat menolak kredensial Wi-Fi');
    }
    const deadline = Date.now() + this.connectTimeoutMs;
    let latest = response.status;
    while (Date.now() < deadline) {
      await this.sleep(this.connectPollIntervalMs);
      latest = await this.status();
      if (latest.state === 'CONNECTED') return latest;
      if (latest.state === 'SETUP_REQUIRED' || latest.state === 'UNCONFIGURED') {
        throw new Error('Perangkat gagal menyimpan kredensial Wi-Fi');
      }
    }
    throw new Error(`Koneksi Wi-Fi tidak selesai (status ${latest.state})`);
  }

  private async request(command: string, payload: Record<string, unknown> = {}, accept: (response: ReturnType<typeof decodeBleResponse>) => boolean = () => true): Promise<ReturnType<typeof decodeBleResponse>> {
    const control = this.control;
    if (!control) throw new Error('Hubungkan perangkat BLE terlebih dahulu');
    const value = encodeBleCommand('', command, payload);
    const requestId = `r${this.nextRequestId++}`;
    const frames = encodeBleFrames(requestId, value);
    return new Promise((resolve, reject) => {
      const timer = globalThis.setTimeout(() => {
        this.pending = null;
        reject(new Error('Perangkat tidak merespons'));
      }, this.requestTimeoutMs);
      this.pending = {
        requestId,
        frames: [],
        accept,
        resolve: (response) => { globalThis.clearTimeout(timer); resolve(response); },
        reject: (error) => { globalThis.clearTimeout(timer); reject(error); },
      };
      void (async () => {
        try {
          for (const frame of frames) await control.writeValue(frame as unknown as BufferSource);
        } catch (error: unknown) {
          globalThis.clearTimeout(timer);
          this.pending = null;
          reject(error instanceof Error ? error : new Error('BLE write gagal'));
        }
      })();
    });
  }
}
