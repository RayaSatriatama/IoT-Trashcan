import { BleProvisioningClient, type BleClientDiagnosticEvent, type BleProvisioningClientOptions } from './client';
import type { BleCalibrationResponse, DebugCursor, DebugLevel, DebugMeta, DebugPage, WifiNetwork, WifiStatus } from './protocol';
import type { BleCalibrationAdapter } from '../../components/device/CalibrationWizard';

export type PublicBleClient = Pick<BleProvisioningClient, keyof BleProvisioningClient>;
export type DeviceBleTrace = Pick<BleClientDiagnosticEvent, 'type' | 'operation' | 'requestId' | 'elapsedMs' | 'chunkIndex' | 'expectedCount' | 'receivedCount' | 'byteLength' | 'reason' | 'category' | 'outcome' | 'lastPhase' | 'attempt' | 'maxAttempts'>;
export type DeviceBleSnapshot = Readonly<{
  connected: boolean; deviceName: string | null; deviceId: string | null;
  busy: 'connect' | 'wifi' | 'calibration' | 'debug' | null; generation: number;
  controlActive: boolean; trace: readonly Readonly<DeviceBleTrace>[];
}>;
export type DeviceBleSessionOptions = { createClient?: (options: BleProvisioningClientOptions) => PublicBleClient };
type LeaseSource = 'ble-calibration' | 'https-calibration';
type Owner = 'wifi' | 'calibration' | 'debug';
const rejected = () => new Error('Device BLE session is disposed');

export class DeviceBleSession {
  private readonly client: PublicBleClient;
  private readonly listeners = new Set<() => void>();
  private readonly leases = new Map<number, { source: LeaseSource; generation: number }>();
  private readonly trace: DeviceBleTrace[] = [];
  private snapshot: DeviceBleSnapshot;
  private disposed = false;
  private connected = false;
  private connectAttempt = 0;
  private intentionalReconnectClose = false;
  private deviceName: string | null = null;
  private deviceId: string | null = null;
  private busy: DeviceBleSnapshot['busy'] = null;
  private generation = 0;
  private nextLease = 0;

  readonly calibrationAdapter: BleCalibrationAdapter;

  constructor(private readonly expectedDeviceId: string | null, options: DeviceBleSessionOptions = {}) {
    const clientOptions: BleProvisioningClientOptions = {
      readOnlyMaxAttempts: 3,
      commandTimeoutMs: { GET_DEVICE_ID: 30_000, GET_STATUS: 30_000, SCAN_WIFI: 30_000, GET_DEBUG: 30_000 },
      onConnectionChange: (connected) => this.connectionChanged(connected),
      onDeviceIdentityChange: (name) => { if (!this.disposed) { this.deviceName = name; this.publish(); } },
      onVerifiedDeviceIdentityChange: (id) => { if (!this.disposed) { this.deviceId = id === this.expectedDeviceId ? id : null; this.publish(); } },
      onDiagnosticEvent: (event) => this.record(event),
    };
    this.client = (options.createClient ?? ((clientOptions) => new BleProvisioningClient(clientOptions)))(clientOptions);
    this.snapshot = this.makeSnapshot();
    const call = <K extends keyof BleCalibrationAdapter>(method: K, ...args: Parameters<NonNullable<BleCalibrationAdapter[K]>>): ReturnType<NonNullable<BleCalibrationAdapter[K]>> => {
      return this.run('calibration', (client) => (client[method] as (...values: unknown[]) => Promise<BleCalibrationResponse>)(...args)) as ReturnType<NonNullable<BleCalibrationAdapter[K]>>;
    };
    this.calibrationAdapter = {
      verifyDeviceIdentity: (id) => this.verifyExpectedIdentity(id),
      calibrationCapabilities: (sensor) => call('calibrationCapabilities', sensor),
      calibrationEvidence: (sensor) => call('calibrationEvidence', sensor),
      calibrationStart: (sensor, parameters) => call('calibrationStart', sensor, parameters),
      calibrationCriteria: (sensor, parameters) => call('calibrationCriteria', sensor, parameters),
      calibrationCapture: (sensor, anchor) => call('calibrationCapture', sensor, anchor),
      calibrationValidate: (sensor) => call('calibrationValidate', sensor),
      calibrationPreview: (sensor) => call('calibrationPreview', sensor),
      calibrationCommit: (sensor, expected) => call('calibrationCommit', sensor, expected),
      calibrationCancel: () => call('calibrationCancel'),
    };
  }

  getSnapshot = (): DeviceBleSnapshot => this.snapshot;
  subscribe = (listener: () => void): (() => void) => { if (this.disposed) return () => {}; this.listeners.add(listener); return () => this.listeners.delete(listener); };

  async connect(): Promise<void> {
    this.requireLive();
    if (!this.expectedDeviceId) throw new Error('Expected BLE device identity is required');
    if (this.busy !== null) throw new Error('A BLE operation is already running');
    const attempt = ++this.connectAttempt;
    this.busy = 'connect'; this.publish();
    try {
      this.intentionalReconnectClose = true;
      let connecting: Promise<void>;
      try { connecting = this.client.connect(); } finally { this.intentionalReconnectClose = false; }
      await connecting;
      this.requireConnectAttempt(attempt);
      this.connected = true;
      const actual = await this.client.verifyDeviceIdentity(this.expectedDeviceId);
      this.requireConnectAttempt(attempt);
      if (actual !== this.expectedDeviceId) throw new Error('Connected BLE device identity does not match the selected device');
      this.deviceId = actual; this.generation += 1;
    } catch (error) {
      if (attempt === this.connectAttempt && !this.disposed) { this.client.close(); this.connected = false; this.deviceId = null; }
      throw error;
    } finally {
      if (attempt === this.connectAttempt) {
        if (!this.disposed) { if (this.busy === 'connect') this.busy = null; this.publish(); }
      }
    }
  }

  async run<T>(owner: Owner, operation: (client: PublicBleClient) => Promise<T>): Promise<T> {
    this.requireLive();
    if (!this.connected || !this.deviceId) throw new Error('A verified BLE connection is required');
    if (this.busy !== null) throw new Error('A BLE operation is already running');
    if (owner === 'debug' && this.leases.size) throw new Error('Debug is paused during calibration activity');
    const generation = this.generation;
    this.busy = owner; this.publish();
    try {
      const result = await operation(this.client);
      if (this.disposed || generation !== this.generation) throw new Error('BLE operation was superseded');
      return result;
    } catch (error) {
      if (this.disposed || generation !== this.generation) throw new Error('BLE operation was superseded');
      throw error;
    } finally {
      if (!this.disposed && generation === this.generation && this.busy === owner) { this.busy = null; this.publish(); }
    }
  }

  close(): void {
    if (this.disposed) return;
    this.generation += 1; this.connectAttempt += 1; this.connected = false; this.deviceId = null; this.deviceName = null; this.busy = null;
    this.leases.forEach((lease, id) => { if (lease.source === 'ble-calibration') this.leases.delete(id); });
    this.client.close(); this.publish();
  }

  dispose(): void {
    if (this.disposed) return;
    this.disposed = true; this.generation += 1; this.connectAttempt += 1; this.connected = false; this.deviceId = null; this.deviceName = null; this.busy = null;
    this.leases.clear(); this.client.close(); this.listeners.clear(); this.snapshot = this.makeSnapshot();
  }

  beginControlActivity(source: LeaseSource): () => void {
    this.requireLive(); const id = ++this.nextLease; const generation = this.generation;
    this.leases.set(id, { source, generation }); this.publish();
    let active = true;
    return () => {
      if (!active) return; active = false;
      const lease = this.leases.get(id);
      if (lease && lease.generation === generation) { this.leases.delete(id); this.publish(); }
    };
  }

  private async verifyExpectedIdentity(id: string): Promise<string> {
    this.requireLive(); if (!this.expectedDeviceId || id !== this.expectedDeviceId) throw new Error('Expected BLE device identity is required');
    return this.run('calibration', (client) => client.verifyDeviceIdentity(id));
  }
  private requireConnectAttempt(attempt: number): void {
    this.requireLive();
    if (attempt !== this.connectAttempt) throw new Error('BLE connection attempt was superseded');
  }
  private connectionChanged(connected: boolean): void {
    if (this.disposed || (!connected && this.intentionalReconnectClose)) return;
    if (!connected) { this.connectAttempt += 1; this.generation += 1; this.connected = false; this.deviceId = null; this.deviceName = null; this.busy = null; this.leases.forEach((lease, id) => { if (lease.source === 'ble-calibration') this.leases.delete(id); }); }
    else this.connected = true;
    this.publish();
  }
  private record(event: BleClientDiagnosticEvent): void {
    const keys: (keyof DeviceBleTrace)[] = ['type','operation','requestId','elapsedMs','chunkIndex','expectedCount','receivedCount','byteLength','reason','category','outcome','lastPhase','attempt','maxAttempts'];
    const item: Record<string, unknown> = {};
    for (const key of keys) {
      const value: unknown = event[key];
      if (typeof value === 'string' || (typeof value === 'number' && Number.isFinite(value))) item[key] = value;
    }
    this.trace.push(Object.freeze(item as DeviceBleTrace)); if (this.trace.length > 60) this.trace.splice(0, this.trace.length - 60); this.publish();
  }
  private makeSnapshot(): DeviceBleSnapshot { return Object.freeze({ connected: this.connected, deviceName: this.deviceName, deviceId: this.deviceId, busy: this.busy, generation: this.generation, controlActive: this.leases.size > 0, trace: Object.freeze(this.trace.slice()) }); }
  private publish(): void {
    this.snapshot = this.makeSnapshot();
    for (const listener of this.listeners) { try { listener(); } catch { /* Subscriber failures cannot disrupt session state. */ } }
  }
  private requireLive(): void { if (this.disposed) throw rejected(); }
}
