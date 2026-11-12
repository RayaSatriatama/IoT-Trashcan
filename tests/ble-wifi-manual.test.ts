import { afterEach, describe, expect, it, vi } from 'vitest';

const mocks = vi.hoisted(() => ({
  options: null as null | Record<string, (value: never) => void>,
  client: null as null | Record<string, (...args: never[]) => unknown>,
}));
vi.mock('../lib/ble/client', () => ({
  BleProvisioningClient: class {
    verifiedDeviceId: string | null = null;
    connected = false;
    constructor(options: typeof mocks.options) { mocks.options = options; mocks.client = this as unknown as typeof mocks.client; }
    connect = vi.fn(async () => {
      if (this.connected) {
        this.connected = false;
        this.verifiedDeviceId = null;
        mocks.options?.onVerifiedDeviceIdentityChange?.(null as never);
        mocks.options?.onConnectionChange?.(false as never);
      }
      this.connected = true;
      mocks.options?.onConnectionChange?.(true as never);
    });
    verifyDeviceIdentity = vi.fn(async () => {
      this.verifiedDeviceId = '000050c1f516a398';
      mocks.options?.onVerifiedDeviceIdentityChange?.(this.verifiedDeviceId as never);
      return this.verifiedDeviceId;
    });
    status = vi.fn(async () => ({ state: 'CONNECTED', ssid: 'saved' }));
    scan = vi.fn(async () => []);
    connectWifi = vi.fn(async () => ({ state: 'CONNECTED', ssid: 'saved' }));
    close = vi.fn(() => {
      if (this.connected) {
        this.connected = false;
        this.verifiedDeviceId = null;
        mocks.options?.onVerifiedDeviceIdentityChange?.(null as never);
        mocks.options?.onConnectionChange?.(false as never);
      }
    });
  },
}));

class ElementStub {
  textContent = '';
  innerHTML = '';
  value = '';
  disabled = false;
  dataset: Record<string, string> = {};
  onclick: (() => void) | null = null;
  children: ElementStub[] = [];
  replaceChildren(...children: ElementStub[]): void { this.children = children; }
  add(child: ElementStub): void { this.children.push(child); }
}
const ids = ['state', 'wifi', 'identity', 'status', 'disconnect', 'scan', 'networks', 'password', 'join', 'connect', 'device', 'trace'];
const originalDocument = Object.getOwnPropertyDescriptor(globalThis, 'document');
const originalOption = Object.getOwnPropertyDescriptor(globalThis, 'Option');
let elements: Map<string, ElementStub>;
async function loadPage(): Promise<void> {
  elements = new Map(ids.map((id) => [id, new ElementStub()]));
  Object.defineProperty(globalThis, 'document', { configurable: true, value: { getElementById: (id: string) => elements.get(id) } });
  Object.defineProperty(globalThis, 'Option', { configurable: true, value: class extends ElementStub { constructor(text: string, value: string) { super(); this.textContent = text; this.value = value; } } });
  mocks.options = null;
  mocks.client = null;
  vi.resetModules();
  await import('../scripts/ble-manual/wifi');
}
async function flush(): Promise<void> {
  await Promise.resolve();
  await Promise.resolve();
  await Promise.resolve();
}
async function connectAndVerify(): Promise<void> {
  elements.get('connect')!.onclick!();
  await flush();
  elements.get('identity')!.onclick!();
  await flush();
}
afterEach(() => {
  if (originalDocument) Object.defineProperty(globalThis, 'document', originalDocument);
  else Reflect.deleteProperty(globalThis, 'document');
  if (originalOption) Object.defineProperty(globalThis, 'Option', originalOption);
  else Reflect.deleteProperty(globalThis, 'Option');
});

describe('candidate Wi-Fi manual page DOM state', () => {
  it('initializes without starting BLE work and keeps only 60 text-only diagnostic events', async () => {
    await loadPage();
    expect(mocks.client!.connect).not.toHaveBeenCalled();
    const trace = elements.get('trace')!;
    trace.innerHTML = 'untouched';
    mocks.options!.onDiagnosticEvent!(
      { type: 'session_disconnected' } as never,
    );
    mocks.options!.onConnectionChange!(true as never);
    for (let index = 0; index < 65; index += 1) {
      mocks.options!.onDiagnosticEvent!({
        type: 'response_fragment', operation: 'SCAN_WIFI', requestId: String(index),
        elapsedMs: index, chunkIndex: 0, expectedCount: 1, receivedCount: 1, byteLength: 4,
      } as never);
    }
    expect(trace.textContent.split('\n')).toHaveLength(60);
    expect(trace.textContent).not.toContain('"requestId":"0"');
    expect(trace.textContent).toContain('"requestId":"64"');
    expect(trace.textContent).not.toContain('SSID');
    expect(trace.innerHTML).toBe('untouched');
  });

  it('clears the password, network selection, results, and status on every disconnect', async () => {
    await loadPage();
    const input = elements.get('password')!;
    const networks = elements.get('networks')!;
    const wifi = elements.get('wifi')!;
    await connectAndVerify();
    input.value = 'local-only-secret';
    networks.value = 'OldNetwork';
    networks.add(new ElementStub());
    wifi.textContent = 'previous status';
    mocks.options!.onConnectionChange(false as never);
    expect(input.value).toBe('');
    expect(networks.value).toBe('');
    expect(networks.children).toHaveLength(1);
    expect(wifi.textContent).toContain('dibersihkan');
  });

  it('discards a scan result that resolves after disconnect', async () => {
    let finishScan!: (result: Array<{ ssid: string; rssi: number; secure: boolean }>) => void;
    await loadPage();
    (mocks.client!.scan as unknown as ReturnType<typeof vi.fn>).mockImplementation(() => new Promise((resolve) => { finishScan = resolve; }));
    await connectAndVerify();
    elements.get('scan')!.onclick!();
    mocks.client!.close!();
    finishScan([{ ssid: 'OldNetwork', rssi: -40, secure: true }]);
    await flush();
    expect(elements.get('networks')!.children).toHaveLength(1);
    expect(elements.get('wifi')!.textContent).not.toContain('OldNetwork');
  });

  it('shows an initial chooser connection failure although no connection was established', async () => {
    await loadPage();
    (mocks.client!.connect as unknown as ReturnType<typeof vi.fn>).mockRejectedValue(new Error('chooser unavailable'));
    elements.get('connect')!.onclick!();
    await flush();
    expect(elements.get('state')!.textContent).toBe('BLE: GAGAL');
  });

  it.each([
    ['identity', 'verifyDeviceIdentity', 'Identitas'],
    ['status', 'status', 'Status'],
    ['scan', 'scan', 'Scan'],
    ['join', 'connectWifi', 'Wi-Fi'],
  ])('keeps %s errors visible after busy cleanup', async (buttonId, method, label) => {
    await loadPage();
    await connectAndVerify();
    const action = mocks.client![method] as unknown as ReturnType<typeof vi.fn>;
    action.mockRejectedValue(new Error('Perangkat tidak merespons'));
    if (buttonId === 'join') elements.get('networks')!.value = 'TestNet';
    elements.get(buttonId)!.onclick!();
    await flush();
    expect(elements.get(buttonId)!.disabled).toBe(false);
    expect(elements.get('state')!.textContent).toBe(`${label}: Perangkat tidak merespons`);
  });

  it('shows a failed reconnect even when connect first closes the previous device', async () => {
    await loadPage();
    await connectAndVerify();
    (mocks.client!.connect as unknown as ReturnType<typeof vi.fn>).mockImplementationOnce(async () => {
      mocks.client!.close!();
      throw new Error('chooser unavailable');
    });
    elements.get('connect')!.onclick!();
    await flush();
    expect(elements.get('state')!.textContent).toBe('BLE: GAGAL');
  });

  it.each(['resolve', 'reject'])('does not let an old join %s erase a newer session password', async (settle) => {
    let finishJoin!: (value: unknown) => void;
    let failJoin!: (error: Error) => void;
    await loadPage();
    (mocks.client!.connectWifi as unknown as ReturnType<typeof vi.fn>).mockImplementation(() => new Promise((resolve, reject) => { finishJoin = resolve; failJoin = reject; }));
    await connectAndVerify();
    elements.get('networks')!.value = 'OldNetwork';
    elements.get('password')!.value = 'old-secret';
    elements.get('join')!.onclick!();
    mocks.client!.close!();
    await flush();
    elements.get('connect')!.onclick!();
    await flush();
    elements.get('identity')!.onclick!();
    await flush();
    elements.get('password')!.value = 'new-session-secret';
    let currentError: string | undefined;
    if (settle === 'reject') {
      (mocks.client!.status as unknown as ReturnType<typeof vi.fn>).mockRejectedValue(new Error('Perangkat tidak merespons'));
      elements.get('status')!.onclick!();
      await flush();
      currentError = elements.get('state')!.textContent;
    }
    if (settle === 'resolve') finishJoin({ state: 'CONNECTED' });
    else failJoin(new Error('Perangkat tidak merespons'));
    await flush();
    expect(elements.get('password')!.value).toBe('new-session-secret');
    if (currentError) expect(elements.get('state')!.textContent).toBe(currentError);
  });
});
