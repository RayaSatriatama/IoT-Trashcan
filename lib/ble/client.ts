import {
  BLE_FRAME_MAX_BYTES,
  decodeBleResponse,
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

const bluetooth = (): BluetoothAdapter => {
  const value = (globalThis as unknown as { navigator?: { bluetooth?: BluetoothAdapter } }).navigator?.bluetooth;
  if (!value || !isWebBluetoothAvailable()) throw new Error('Web Bluetooth membutuhkan HTTPS dan browser yang mendukungnya');
  return value;
};

export class BleProvisioningClient {
  private device: BleDevice | null = null;
  private server: BleServer | null = null;
  private control: BleCharacteristic | null = null;
  private pending = new Map<string, { resolve: (response: ReturnType<typeof decodeBleResponse>) => void; reject: (error: Error) => void }>();
  private sequence = 0;

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
        const response = decodeBleResponse(new Uint8Array(value.buffer, value.byteOffset, value.byteLength));
        const waiter = this.pending.get(response.id);
        if (!waiter) return;
        this.pending.delete(response.id);
        if (response.ok) waiter.resolve(response);
        else waiter.reject(new Error(response.error || 'Perangkat menolak permintaan'));
      } catch {
        // Invalid frames are ignored; the request timeout provides bounded recovery.
      }
    });
    this.device = device;
    device.addEventListener('gattserverdisconnected', () => this.close());
  }

  close(): void {
    for (const waiter of this.pending.values()) waiter.reject(new Error('BLE terputus'));
    this.pending.clear();
    this.control = null;
    this.server?.disconnect?.();
    this.server = null;
    this.device = null;
  }

  async status(): Promise<WifiStatus> {
    const response = await this.request('STATUS');
    if (!response.status) throw new Error('Status WiFi tidak tersedia');
    return response.status;
  }

  async scan(): Promise<WifiNetwork[]> {
    const response = await this.request('SCAN');
    return response.networks ?? [];
  }

  async connectWifi(ssid: string, password: string): Promise<WifiStatus> {
    if (!ssid.trim() || ssid.length > 32 || password.length > 63) throw new Error('SSID atau password tidak valid');
    const response = await this.request('CONNECT', { ssid, password });
    if (!response.status) throw new Error('Status koneksi tidak tersedia');
    return response.status;
  }

  private async request(command: string, payload: Record<string, unknown> = {}): Promise<ReturnType<typeof decodeBleResponse>> {
    const control = this.control;
    if (!control) throw new Error('Hubungkan perangkat BLE terlebih dahulu');
    const id = `web-${++this.sequence}`;
    const value = encodeBleCommand(id, command, payload);
    return new Promise((resolve, reject) => {
      const timer = globalThis.setTimeout(() => {
        this.pending.delete(id);
        reject(new Error('Perangkat tidak merespons'));
      }, 5000);
      this.pending.set(id, {
        resolve: (response) => { globalThis.clearTimeout(timer); resolve(response); },
        reject: (error) => { globalThis.clearTimeout(timer); reject(error); },
      });
      if (value.byteLength > BLE_FRAME_MAX_BYTES) {
        globalThis.clearTimeout(timer);
        this.pending.delete(id);
        reject(new Error('BLE command is too large'));
        return;
      }
      void control.writeValue(value as unknown as BufferSource).catch((error: unknown) => {
        globalThis.clearTimeout(timer);
        this.pending.delete(id);
        reject(error instanceof Error ? error : new Error('BLE write gagal'));
      });
    });
  }
}
