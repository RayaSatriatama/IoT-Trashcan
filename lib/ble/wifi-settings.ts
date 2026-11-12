import type { DeviceBleSession } from './device-session';
import type { WifiNetwork, WifiStatus } from './protocol';

export type WifiSettingsSnapshot = Readonly<{
  open: boolean; status: WifiStatus | null; nearby: readonly WifiNetwork[]; saved: readonly WifiNetwork[];
  rawCount: number; busy: boolean; readonlyBusy: boolean; error: string | null; success: string | null;
  selectedSsid: string; password: string; deviceId: string | null;
}>;

const validStates = ['SETUP_REQUIRED', 'CONNECTING', 'CONNECTED', 'BACKOFF', 'UNCONFIGURED'];
const validStatus = (value: unknown): value is WifiStatus => {
  if (!value || typeof value !== 'object') return false;
  const status = value as WifiStatus;
  return validStates.includes(status.state) && (status.ssid === null || typeof status.ssid === 'string');
};

export class WifiSettingsController {
  private isOpen = false;
  private status: WifiStatus | null = null;
  private nearby: WifiNetwork[] = [];
  private saved: WifiNetwork[] = [];
  private rawCount = 0;
  private busy = false;
  private readonlyBusy = false;
  private error: string | null = null;
  private success: string | null = null;
  private selectedSsid = '';
  private password = '';
  private listeners = new Set<() => void>();
  private generation = 0;
  private operationId = 0;
  private activeOperation: number | null = null;
  private connectAttempt = 0;
  private disposed = false;
  private unsubscribeSession: () => void;
  private snapshot: WifiSettingsSnapshot;

  constructor(private readonly session: DeviceBleSession) {
    this.snapshot = this.makeSnapshot();
    this.unsubscribeSession = session.subscribe(() => this.sessionChanged());
  }
  getSnapshot = (): WifiSettingsSnapshot => this.snapshot;
  subscribe = (listener: () => void): (() => void) => { this.listeners.add(listener); return () => this.listeners.delete(listener); };
  open(): void { if (this.disposed) return; this.isOpen = true; this.publish(); }
  openDialog(): void { this.open(); }
  openSettings(): void { this.open(); }
  close(): void { if (this.disposed) return; this.generation++; this.isOpen = false; this.password = ''; this.error = null; this.success = null; this.publish(); }
  setPassword(password: string): void { if (this.disposed) return; this.password = password; this.publish(); }
  selectSsid(ssid: string): void { if (this.disposed) return; this.selectedSsid = ssid; this.publish(); }
  connectBle(): Promise<void> {
    if (this.disposed) return Promise.reject(new Error('Wi-Fi settings controller is disposed'));
    if (this.session.getSnapshot().busy !== null) return this.session.connect();
    const attempt = ++this.connectAttempt;
    ++this.generation;
    this.password = '';
    this.error = null;
    this.success = null;
    this.publish();
    return this.session.connect().then(() => {
      if (attempt !== this.connectAttempt || this.disposed) return;
    }).catch((error: unknown) => {
      if (attempt === this.connectAttempt && !this.disposed) {
        this.error = error instanceof Error ? error.message : 'BLE connection failed';
        this.publish();
      }
      throw error;
    });
  }
  dispose(): void {
    if (this.disposed) return;
    this.disposed = true;
    this.connectAttempt++;
    this.generation++;
    this.password = '';
    this.activeOperation = null;
    this.busy = false;
    this.readonlyBusy = false;
    this.unsubscribeSession();
    this.publish();
    this.listeners.clear();
  }
  async scan(): Promise<void> {
    if (this.disposed) return;
    await this.perform(async (client) => client.scanDetailed(), (result) => {
      this.nearby = result.networks;
      this.rawCount = result.rawCount;
      this.selectedSsid = result.networks[0]?.ssid ?? '';
    });
  }
  async loadStatus(): Promise<void> {
    if (this.disposed) return;
    await this.perform(async (client) => client.status(), (status) => {
      if (!validStatus(status)) throw new Error('Invalid Wi-Fi status response');
      this.status = status;
    });
  }
  async loadSaved(): Promise<void> {
    if (this.disposed) return;
    await this.perform(async (client) => client.listKnownWifi(), (saved) => {
      this.saved = saved;
    });
  }
  async submit(): Promise<void> {
    if (this.disposed) return;
    const ssid = this.selectedSsid;
    const password = this.password;
    this.password = '';
    this.publish();
    if (!ssid) {
      this.error = 'Select a Wi-Fi network';
      this.publish();
      return;
    }
    await this.perform(async (client) => client.connectWifi(ssid, password), (status) => {
      if (!validStatus(status)) throw new Error('Invalid Wi-Fi status response');
      this.status = status;
      this.success = status.state === 'CONNECTED' ? 'Connected' : null;
    }, false);
  }
  async retrySaved(): Promise<void> {
    if (this.disposed) return;
    await this.perform(async (client) => client.retryWifi(), (status) => {
      if (!validStatus(status)) throw new Error('Invalid Wi-Fi status response');
      this.status = status;
      this.success = status.state === 'CONNECTED' ? 'Connected' : null;
    }, false);
  }
  retry(): Promise<void> { return this.retrySaved(); }
  private async perform<T>(
    operation: (client: Parameters<Parameters<DeviceBleSession['run']>[1]>[0]) => Promise<T>,
    commit: (result: T) => void,
    readonly = true,
  ): Promise<void> {
    const revision = this.generation;
    const peerGeneration = this.session.getSnapshot().generation;
    const ownsOperation = this.session.getSnapshot().busy === null;
    const operationId = ownsOperation ? ++this.operationId : this.activeOperation;
    if (ownsOperation) {
      this.activeOperation = operationId;
      this.busy = true;
      this.readonlyBusy = readonly;
    }
    this.error = null;
    this.success = null;
    this.publish();
    try {
      const result = await this.session.run('wifi', operation);
      if (revision !== this.generation || peerGeneration !== this.session.getSnapshot().generation || this.disposed) return;
      commit(result);
    } catch (error) {
      if (revision === this.generation && peerGeneration === this.session.getSnapshot().generation && !this.disposed) {
        this.error = error instanceof Error ? error.message : 'Wi-Fi operation failed';
      }
    } finally {
      if (ownsOperation && operationId === this.activeOperation) {
        this.activeOperation = null;
        this.busy = false;
        this.readonlyBusy = false;
        this.publish();
      }
    }
  }
  private sessionChanged(): void {
    if (!this.session.getSnapshot().connected) {
      this.generation++;
      this.password = '';
      if (this.activeOperation === null) {
        this.busy = false;
        this.readonlyBusy = false;
      }
    }
    this.publish();
  }
  private makeSnapshot(): WifiSettingsSnapshot {
    return Object.freeze({ open: this.isOpen, status: this.status, nearby: Object.freeze([...this.nearby]), saved: Object.freeze([...this.saved]), rawCount: this.rawCount, busy: this.busy, readonlyBusy: this.readonlyBusy, error: this.error, success: this.success, selectedSsid: this.selectedSsid, password: this.password, deviceId: this.session.getSnapshot().deviceId });
  }
  private publish(): void { this.snapshot = this.makeSnapshot(); for (const listener of this.listeners) listener(); }
}
