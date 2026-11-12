import { BleProvisioningClient, type BleClientDiagnosticEvent } from '../../lib/ble/client';

const get = (id: string) => document.getElementById(id) as HTMLInputElement;
const state = get('state');
const wifi = get('wifi');
let connected = false;
let generation = 0;
let busy = false;
let feedback: string | null = null;
let explicitConnectStarting = false;
let diagnostics: BleClientDiagnosticEvent[] = [];
const trace = document.getElementById('trace') as HTMLElement;
const recordDiagnostic = (event: BleClientDiagnosticEvent): void => {
  diagnostics = [...diagnostics, event].slice(-60);
  trace.textContent = diagnostics.length ? diagnostics.map((entry) => JSON.stringify(entry)).join('\n') : 'Belum ada kejadian BLE.';
};
const resetDisconnectedState = (keepBusy = false): void => {
  generation += 1;
  if (!keepBusy) busy = false;
  feedback = null;
  get('password').value = '';
  const networks = document.getElementById('networks') as HTMLSelectElement;
  networks.replaceChildren(new Option('Pilih jaringan', ''));
  networks.value = '';
  wifi.textContent = 'Koneksi terputus; status dan pilihan jaringan dibersihkan.';
  state.textContent = 'Belum terhubung';
};
const client = new BleProvisioningClient({
  requestTimeoutMs: 30_000,
  readOnlyMaxAttempts: 3,
  onConnectionChange: (value) => {
    connected = value;
    if (value) {
      diagnostics = [];
      trace.textContent = 'Sesi baru; menunggu kejadian BLE.';
    }
    if (!value) resetDisconnectedState(explicitConnectStarting);
    render();
  },
  onDeviceIdentityChange: (name) => { get('device').textContent = name ?? 'Belum terhubung'; },
  onVerifiedDeviceIdentityChange: (id) => { get('identity').dataset.verified = id ?? ''; render(); },
  onDiagnosticEvent: recordDiagnostic,
});
const verified = () => client.verifiedDeviceId === '000050c1f516a398';
function render(): void {
  get('identity').disabled = !connected || busy;
  get('status').disabled = !verified() || busy;
  get('disconnect').disabled = !connected;
  get('scan').disabled = !verified() || busy;
  get('networks').disabled = !verified() || busy;
  get('password').disabled = !verified() || busy;
  get('join').disabled = !verified() || busy;
  get('connect').disabled = busy;
  state.textContent = feedback ?? (connected ? (verified() ? `Terhubung; ID cocok: ${client.verifiedDeviceId}` : 'Terhubung; verifikasi ID diperlukan') : 'Belum terhubung');
}
function report(label: string, error?: unknown): void {
  const name = error instanceof Error ? error.message : '';
  const allowed = new Set(['Perangkat gagal menyimpan kredensial Wi-Fi','Perangkat menolak kredensial Wi-Fi','Perangkat tidak merespons','Identitas perangkat BLE belum diverifikasi','Hubungkan perangkat BLE terlebih dahulu','Perangkat BLE tidak cocok dengan device_id yang dipilih']);
  feedback = error && allowed.has(name) ? `${label}: ${name}` : `${label}: ${error ? 'GAGAL' : 'selesai'}`;
  state.textContent = feedback;
}
async function operate<T>(label: string, action: () => Promise<T>, success?: (value: T) => void, allowDisconnectedError = false, rebaseAfterStart = false): Promise<void> {
  if (busy) return;
  let operation = generation;
  feedback = null;
  busy = true;
  render();
  try {
    const pending = action();
    if (rebaseAfterStart) operation = generation;
    const result = await pending;
    if (operation !== generation || !connected) return;
    success?.(result);
    report(label);
  } catch (error) {
    if (operation === generation && (connected || allowDisconnectedError)) report(label, error);
  } finally {
    if (operation === generation) {
      busy = false;
      render();
    }
  }
}
get('connect').onclick = () => { void operate('BLE', () => {
  explicitConnectStarting = true;
  try { return client.connect(); } finally { explicitConnectStarting = false; }
}, undefined, true, true); };
get('identity').onclick = () => { void operate('Identitas', () => client.verifyDeviceIdentity('000050c1f516a398')); };
get('status').onclick = () => { void operate('Status', () => client.status(), (s) => { wifi.textContent = `Status Wi-Fi: ${s.state}${s.ssid ? `, SSID ${s.ssid}` : ''}`; }); };
get('scan').onclick = () => { void operate('Scan', () => client.scan(), (items) => {
  const select = document.getElementById('networks') as HTMLSelectElement;
  select.replaceChildren(new Option('Pilih jaringan', ''));
  for (const item of items) select.add(new Option(`${item.ssid} (${item.rssi} dBm)${item.secure ? ' · secured' : ''}`, item.ssid));
  wifi.textContent = `Scan selesai: ${items.length} jaringan`;
}); };
get('join').onclick = () => {
  const select = get('networks');
  const password = get('password');
  const ssid = select.value;
  const secret = password.value;
  password.value = '';
  void operate('Wi-Fi', async () => {
    if (!ssid) throw new Error('Pilih SSID hasil scan');
    return client.connectWifi(ssid, secret);
  }, (s) => { wifi.textContent = `Hasil: ${s.state}${s.ssid ? `, SSID ${s.ssid}` : ''}`; });
};
get('disconnect').onclick = () => { client.close(); };
render();
