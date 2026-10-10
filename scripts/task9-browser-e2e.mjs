import { pathToFileURL } from 'node:url';
import { spawn } from 'node:child_process';

const endpoint = process.env.SIGAP_E2E_URL;
const core = process.env.SIGAP_PLAYWRIGHT_CORE;
const chromiumPath = process.env.SIGAP_CHROMIUM ?? '/usr/bin/chromium';
const startExpo = process.env.SIGAP_E2E_START_EXPO === '1';

if (!endpoint || !core) {
  throw new Error('SIGAP_E2E_URL and SIGAP_PLAYWRIGHT_CORE are required');
}

const playwrightModule = await import(pathToFileURL(core).href);
const { chromium } = playwrightModule.default ?? playwrightModule;
const proposalHash = `sha256:${'a'.repeat(64)}`;
const evidenceHash = `sha256:${'b'.repeat(64)}`;

async function waitForExpo(url) {
  const deadline = Date.now() + 60_000;
  while (Date.now() < deadline) {
    try {
      const response = await fetch(url);
      if (response.ok) return;
    } catch {}
    await new Promise((resolve) => setTimeout(resolve, 250));
  }
  throw new Error(`Expo web server did not become ready: ${url}`);
}

let expo = null;
if (startExpo) {
  const port = new URL(endpoint).port;
  if (!port) throw new Error('SIGAP_E2E_URL must include an explicit port when starting Expo');
  expo = spawn('npm', ['run', 'web', '--', '--port', port], {
    env: { ...process.env, CI: '1' }, stdio: 'ignore',
  });
  await waitForExpo(endpoint);
}

function device(bleDeviceId) {
  return {
    id: 'device-a', location_id: 'location-a', device_code: 'SIGAP-TEST-01',
    ble_device_id: bleDeviceId, placement_label: 'Synthetic browser E2E',
    online_state: 'Online', last_seen: '2026-10-10T00:00:00Z', status: 'Normal', slots: [],
  };
}

function research() {
  return {
    device_id: 'device-a', decision_summary: { priority_score: null, critical_stream: null, critical_reason: null, mode: null },
    slots: [{
      stream: 'organic', recorded_at: '2026-10-10T00:00:00Z',
      raw: { fill: null, weight: null, gas_raw: 123, temperature: null },
      normalized: { fill_pct: null, weight_utilization: null, ogi: null, accumulation_hours: null, temperature: null },
      sensor_health: null, fis_score: null, fis_status: null, fis_version: null, dominant_reason: null, safety_state: null,
      sensors: [{ sensor_id: 'hx711_organic', sensor_type: 'hx711', parameter: 'weight', raw_value: 123, engineering_value: null, normalized_value: null, raw_unit: 'counts', unit: 'kg', sensor_state: 'VALID' }],
      derived_parameters: { ogi: null, accumulation_hours: null }, fis_trace: null, safety: null,
    }],
  };
}

function installSyntheticGatt({ identity, proposalHash, evidenceHash }) {
    const decoder = new TextDecoder();
    const encoder = new TextEncoder();
    const incoming = new Map();
    const listeners = [];
    const events = {
      async startNotifications() { return this; },
      addEventListener(_type, listener) { listeners.push(listener); },
    };
    const encodeFrames = (requestId, text) => {
      const payload = encoder.encode(text);
      let count = 1;
      for (;;) {
        const parts = [];
        let offset = 0;
        while (offset < payload.length || parts.length === 0) {
          const header = encoder.encode(`R1|${requestId}|${parts.length}|${count}|`);
          const end = Math.min(payload.length, offset + 20 - header.length);
          parts.push(payload.slice(offset, end));
          offset = end;
          if (end === payload.length) break;
        }
        if (parts.length === count) {
          return parts.map((part, index) => {
            const header = encoder.encode(`R1|${requestId}|${index}|${count}|`);
            const frame = new Uint8Array(header.length + part.length);
            frame.set(header); frame.set(part, header.length);
            return frame;
          });
        }
        count = parts.length;
      }
    };
    const reply = (requestId, text) => {
      window.__sigapBleResponses.push(text);
      encodeFrames(requestId, text).forEach((frame) => {
        const value = new DataView(frame.buffer, frame.byteOffset, frame.byteLength);
        listeners.forEach((listener) => listener({ target: { value } }));
      });
    };
    const calibration = (operation, sensor, data) => `CAL1|op=${operation}|status=ACCEPTED|session=42|sensor=${sensor}|data=${data}`;
    const commandReply = (requestId, command) => {
      window.__sigapBleCommands.push(command);
      const normalized = command.trim();
      if (normalized === 'GET_DEVICE_ID') return reply(requestId, `OK DEVICE_ID id=${identity}`);
      const parts = normalized.split(/\s+/u);
      if (parts[0] !== 'CAL') return reply(requestId, 'ERR UNSUPPORTED');
      const [_, operation, sensor] = parts;
      if (operation === 'CAPABILITIES') return reply(requestId, calibration(operation, sensor, 'references=tare_counts:counts,reference_mass_mg:mg;criteria=samples:count,max_span_counts:counts;engineering=none'));
      if (operation === 'EVIDENCE') return reply(requestId, calibration(operation, sensor, 'state=IDLE;s=1;st=organic;q=UNCALIBRATED;r=PROFILE_MISSING;raw=1;fr=FRESH;f=id=1,crc=1,conv=1;p=0;a=0;x=0'));
      if (operation === 'START') return reply(requestId, calibration(operation, sensor, 'state=ACTIVE'));
      if (operation === 'CRITERIA') return reply(requestId, calibration(operation, sensor, 'state=CRITERIA_SET'));
      if (operation === 'CAPTURE') return reply(requestId, calibration(operation, sensor, `state=CAPTURING;evidence_count=3;evidence_hash=${evidenceHash}`));
      if (operation === 'VALIDATE') return reply(requestId, calibration(operation, sensor, `state=VALIDATED;evidence_count=3;evidence_hash=${evidenceHash}`));
      if (operation === 'PREVIEW') return reply(requestId, calibration(operation, sensor, `state=PREVIEW_READY;proposal_hash=${proposalHash};profile_version=7;evidence_count=3;evidence_hash=${evidenceHash};expires_in_ms=60000`));
      if (operation === 'COMMIT') return reply(requestId, calibration(operation, sensor, `state=COMMITTED;profile_key=${sensor};profile_hash=${proposalHash};profile_version=7`));
      return reply(requestId, `CAL1|op=${operation}|status=ERROR|session=42|sensor=${sensor}|data=error=UNSUPPORTED`);
    };
    const control = {
      async writeValue(value) {
        const bytes = value instanceof ArrayBuffer ? new Uint8Array(value) : new Uint8Array(value.buffer, value.byteOffset, value.byteLength);
        const indexes = [];
        for (let index = 0; index < bytes.length && indexes.length < 4; index += 1) if (bytes[index] === 0x7c) indexes.push(index);
        const requestId = decoder.decode(bytes.slice(indexes[0] + 1, indexes[1]));
        const chunkIndex = Number(decoder.decode(bytes.slice(indexes[1] + 1, indexes[2])));
        const chunkCount = Number(decoder.decode(bytes.slice(indexes[2] + 1, indexes[3])));
        const frames = incoming.get(requestId) ?? [];
        frames[chunkIndex] = bytes.slice(indexes[3] + 1);
        incoming.set(requestId, frames);
        if (frames.filter(Boolean).length !== chunkCount) return;
        incoming.delete(requestId);
        commandReply(requestId, decoder.decode(new Uint8Array(frames.reduce((sum, part) => sum + part.length, 0)).map((_, index) => {
          let offset = 0;
          for (const part of frames) { if (index >= offset && index < offset + part.length) return part[index - offset]; offset += part.length; }
          return 0;
        })));
      },
    };
    const server = { async getPrimaryService() { return { async getCharacteristic(uuid) { return uuid.includes('0002-') ? control : events; } }; } };
    const disconnected = [];
    const device = { name: 'spoofable-name', gatt: { async connect() { return server; }, disconnect() {} }, addEventListener(_type, listener) { disconnected.push(listener); } };
    window.__sigapBleCommands = [];
    window.__sigapBleResponses = [];
    window.__sigapDisconnect = () => disconnected.forEach((listener) => listener());
    Object.defineProperty(navigator, 'bluetooth', { configurable: true, value: { async requestDevice() { return device; } } });
}

async function routeApi(page, bleDeviceId) {
  await page.route('**/api/**', async (route) => {
    const path = new URL(route.request().url()).pathname;
    const payload = path === '/api/devices/device-a' ? device(bleDeviceId)
      : path === '/api/devices/device-a/research' ? research()
        : path === '/api/runtime' ? { simulation: false, simulationIndicator: false }
        : { error: 'unexpected synthetic browser request' };
    await route.fulfill({ status: payload.error ? 404 : 200, contentType: 'application/json', body: JSON.stringify(payload) });
  });
}

async function open(page) {
  await page.goto(`${endpoint.replace(/\/$/u, '')}/devices/device-a`, { waitUntil: 'domcontentloaded', timeout: 60_000 });
  await page.getByText('SIGAP-TEST-01').waitFor({ timeout: 60_000 });
}

// The Expo dev server is loopback HTTP. This browser-only synthetic harness
// marks that exact origin secure so Web Bluetooth capability gating can be
// rendered; it neither disables TLS checks nor represents a device pairing.
const browser = await chromium.launch({
  executablePath: chromiumPath,
  headless: true,
  // The runner is local-only. HTTPS is still required for the Web Bluetooth
  // secure-context gate; certificate trust is limited to this disposable run.
  args: ['--ignore-certificate-errors'],
});
const errors = [];
try {
  const missing = await browser.newPage({ viewport: { width: 1920, height: 1080 } });
  await routeApi(missing, null);
  await open(missing);
  console.log('browser-e2e: missing mapping rendered');
  await missing.getByText('BLE belum diprovision').waitFor();
  await missing.screenshot({ path: '/tmp/sigap-task9-browser-missing-identity.png', fullPage: true });
  await missing.close();

  const context = await browser.newContext({ viewport: { width: 1920, height: 1080 } });
  await context.addInitScript(installSyntheticGatt, {
    identity: '0000000000000042', proposalHash, evidenceHash,
  });
  const page = await context.newPage();
  page.on('console', (entry) => { if (entry.type() === 'error') errors.push(entry.text()); });
  await routeApi(page, '0000000000000042');
  await open(page);
  console.log('browser-e2e: verified mapping rendered');
  await page.getByLabel('Kalibrasi hx711_organic').nth(1).click();
  await page.getByLabel('Hubungkan BLE kalibrasi').click();
  console.log('browser-e2e: BLE connect requested');
  try {
    await page.getByText('Bukti raw segar tersedia untuk validasi.').waitFor({ timeout: 15_000 });
  } catch (error) {
    console.error('browser-e2e diagnostic state', await page.evaluate(() => ({
      secure: globalThis.isSecureContext,
      bluetooth: typeof navigator.bluetooth,
      commands: window.__sigapBleCommands,
      responses: window.__sigapBleResponses,
      text: document.body.innerText.slice(0, 3000),
    })));
    throw error;
  }
  console.log('browser-e2e: BLE identity and evidence accepted');
  await page.getByLabel('Referensi tare_counts').fill('123');
  await page.getByLabel('Referensi reference_mass_mg').fill('1000');
  await page.getByLabel('Mulai kalibrasi').click();
  console.log('browser-e2e: start submitted');
  await page.getByLabel('Kriteria samples').fill('3');
  await page.getByLabel('Kriteria max_span_counts').fill('20');
  await page.getByLabel('Simpan kriteria').click();
  console.log('browser-e2e: criteria submitted');
  await page.getByLabel('Anchor kalibrasi').fill('tare');
  await page.getByLabel('Ambil sampel').click();
  console.log('browser-e2e: capture submitted');
  try {
    await page.getByLabel('Validasi sampel').click({ timeout: 15_000 });
  } catch (error) {
    console.error('browser-e2e capture state', await page.evaluate(() => ({
      commands: window.__sigapBleCommands,
      responses: window.__sigapBleResponses,
      text: document.body.innerText.slice(-1800),
    })));
    throw error;
  }
  console.log('browser-e2e: validate submitted');
  await page.getByLabel('Buat preview').click();
  console.log('browser-e2e: preview submitted');
  await page.getByText('Preview proposal').waitFor();
  await page.getByLabel('Commit profil').click();
  console.log('browser-e2e: commit submitted');
  await page.getByText('Langkah: done').waitFor();
  const state = await page.evaluate(() => ({ commands: window.__sigapBleCommands, local: localStorage.length, session: sessionStorage.length }));
  if (!state.commands.some((value) => value.includes(`expected_profile_hash=${proposalHash}`)) || !state.commands.some((value) => value.includes('expected_profile_version=7'))) {
    throw new Error('commit did not preserve the preview identity');
  }
  if (state.local !== 0) throw new Error('browser persisted credentials in localStorage');
  await page.evaluate(() => window.__sigapDisconnect());
  await page.getByText('BLE belum terhubung.').waitFor();
  await page.screenshot({ path: '/tmp/sigap-task9-browser-ble-journey.png', fullPage: true });
  await context.close();

  const wrong = await browser.newContext({ viewport: { width: 1920, height: 1080 } });
  await wrong.addInitScript(installSyntheticGatt, {
    identity: '00000000000000ff', proposalHash, evidenceHash,
  });
  const wrongPage = await wrong.newPage();
  await routeApi(wrongPage, '0000000000000042');
  await open(wrongPage);
  await wrongPage.getByLabel('Kalibrasi hx711_organic').nth(1).click();
  await wrongPage.getByLabel('Hubungkan BLE kalibrasi').click();
  console.log('browser-e2e: wrong identity requested');
  await wrongPage.getByText('Perangkat BLE tidak cocok dengan device_id yang dipilih').waitFor({ timeout: 15_000 });
  await wrong.close();

  if (errors.length) throw new Error(`browser console errors: ${errors.join('; ')}`);
  console.log('Task 9 browser synthetic E2E: PASS (missing mapping, verified lifecycle, disconnect, wrong identity)');
} finally {
  await browser.close();
  expo?.kill('SIGTERM');
}
