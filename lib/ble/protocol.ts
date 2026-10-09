export const SIGAP_BLE_SERVICE_UUID = '7d3f0001-7d3f-4a53-9a4b-534947415000';
export const SIGAP_BLE_CONTROL_UUID = '7d3f0002-7d3f-4a53-9a4b-534947415000';
export const SIGAP_BLE_EVENTS_UUID = '7d3f0003-7d3f-4a53-9a4b-534947415000';

export const BLE_FRAME_MAX_BYTES = 512;
export const BLE_ATT_FRAME_BYTES = 20;
export const BLE_REQUEST_FRAME_VERSION = 'F1';
export const BLE_RESPONSE_FRAME_VERSION = 'R1';

export type BleFrame = {
  version: typeof BLE_REQUEST_FRAME_VERSION | typeof BLE_RESPONSE_FRAME_VERSION;
  requestId: string;
  chunkIndex: number;
  chunkCount: number;
  payload: Uint8Array;
};

export type WifiNetwork = {
  ssid: string;
  rssi: number;
  secure: boolean;
};

export type WifiStatus = {
  state: 'SETUP_REQUIRED' | 'CONNECTING' | 'CONNECTED' | 'BACKOFF' | 'UNCONFIGURED';
  ssid: string | null;
};

export type BleResponse = {
  ok: boolean;
  error?: string;
  status?: WifiStatus;
  networks?: WifiNetwork[];
  scanPending?: boolean;
  calibration?: BleCalibrationResponse;
};

export type BleCalibrationStatus = 'ACCEPTED' | 'ERROR';
export type BleCalibrationResponse = {
  version: 'CAL1';
  operation: BleCalibrationOperation | 'PROFILE';
  status: BleCalibrationStatus;
  sessionId: string;
  sensor: string | null;
  data: Record<string, string>;
  error?: string;
};

export const BLE_CALIBRATION_OPERATIONS = [
  'CAPABILITIES', 'STATUS', 'EVIDENCE', 'START', 'CRITERIA', 'CAPTURE',
  'VALIDATE', 'PREVIEW', 'COMMIT', 'CANCEL', 'HISTORY', 'ROLLBACK', 'ACTIVE_PROFILE',
] as const;
export type BleCalibrationOperation = typeof BLE_CALIBRATION_OPERATIONS[number];

function validateFrameHeader(version: string, requestId: string, chunkIndex: number, chunkCount: number): void {
  if (version !== BLE_REQUEST_FRAME_VERSION && version !== BLE_RESPONSE_FRAME_VERSION) throw new Error('BLE frame version invalid');
  if (!/^\d{1,10}$/u.test(requestId)) throw new Error('BLE request id invalid');
  if (!Number.isInteger(chunkIndex) || !Number.isInteger(chunkCount) || chunkCount < 1 || chunkIndex < 0 || chunkIndex >= chunkCount) {
    throw new Error('BLE chunk bounds invalid');
  }
}

function splitPayload(payload: Uint8Array, requestId: string, version: typeof BLE_REQUEST_FRAME_VERSION | typeof BLE_RESPONSE_FRAME_VERSION): Uint8Array[] {
  let chunkCount = 1;
  for (;;) {
    const chunks: Uint8Array[] = [];
    let offset = 0;
    while (offset < payload.byteLength || chunks.length === 0) {
      const headerBytes = new TextEncoder().encode(`${version}|${requestId}|${chunks.length}|${chunkCount}|`);
      const available = BLE_ATT_FRAME_BYTES - headerBytes.byteLength;
      if (available <= 0) throw new Error('BLE frame header is too large');
      const end = Math.min(payload.byteLength, offset + available);
      chunks.push(payload.slice(offset, end));
      offset = end;
      if (end === payload.byteLength) break;
    }
    if (chunks.length === chunkCount) {
      return chunks.map((chunk, index) => {
        const header = new TextEncoder().encode(`${version}|${requestId}|${index}|${chunkCount}|`);
        const frame = new Uint8Array(header.byteLength + chunk.byteLength);
        frame.set(header);
        frame.set(chunk, header.byteLength);
        return frame;
      });
    }
    chunkCount = chunks.length;
  }
}

export function encodeBleFrames(
  requestId: string,
  payload: Uint8Array,
  version: typeof BLE_REQUEST_FRAME_VERSION | typeof BLE_RESPONSE_FRAME_VERSION = BLE_REQUEST_FRAME_VERSION,
): Uint8Array[] {
  validateFrameHeader(version, requestId, 0, 1);
  if (payload.byteLength > BLE_FRAME_MAX_BYTES) throw new Error('BLE payload is too large');
  return splitPayload(payload, requestId, version);
}

export function decodeBleFrame(bytes: Uint8Array): BleFrame {
  if (bytes.byteLength === 0 || bytes.byteLength > BLE_ATT_FRAME_BYTES) throw new Error('BLE frame size invalid');
  const text = new TextDecoder('utf-8', { fatal: true }).decode(bytes);
  const first = text.indexOf('|');
  const second = text.indexOf('|', first + 1);
  const third = text.indexOf('|', second + 1);
  const fourth = text.indexOf('|', third + 1);
  if (first <= 0 || second <= first || third <= second || fourth <= third) throw new Error('BLE frame envelope invalid');
  const version = text.slice(0, first) as BleFrame['version'];
  const requestId = text.slice(first + 1, second);
  const chunkIndex = Number(text.slice(second + 1, third));
  const chunkCount = Number(text.slice(third + 1, fourth));
  validateFrameHeader(version, requestId, chunkIndex, chunkCount);
  const payload = new TextEncoder().encode(text.slice(fourth + 1));
  if (bytes.byteLength > BLE_ATT_FRAME_BYTES) throw new Error('BLE frame size invalid');
  return { version, requestId, chunkIndex, chunkCount, payload };
}

export function assembleBleFrames(
  frames: readonly BleFrame[],
  expectedVersion: BleFrame['version'] = BLE_RESPONSE_FRAME_VERSION,
): Uint8Array {
  if (frames.length === 0) throw new Error('BLE response is empty');
  const first = frames[0];
  if (first.version !== expectedVersion || frames.length !== first.chunkCount) throw new Error('BLE response chunks incomplete');
  const ordered = [...frames].sort((left, right) => left.chunkIndex - right.chunkIndex);
  const output = new Uint8Array(ordered.reduce((sum, frame) => sum + frame.payload.byteLength, 0));
  let offset = 0;
  ordered.forEach((frame, index) => {
    if (frame.version !== first.version || frame.requestId !== first.requestId || frame.chunkCount !== first.chunkCount || frame.chunkIndex !== index) {
      throw new Error('BLE response chunks inconsistent');
    }
    output.set(frame.payload, offset);
    offset += frame.payload.byteLength;
  });
  if (output.byteLength > BLE_FRAME_MAX_BYTES) throw new Error('BLE response is too large');
  return output;
}

export function encodeBleCommand(_id: string, command: string, payload: Record<string, unknown> = {}): Uint8Array {
  let body = command.trim();
  const normalized = body.toUpperCase();
  if (normalized === 'CONNECT' || normalized === 'CONNECT_WIFI') {
    const ssid = typeof payload.ssid === 'string' ? payload.ssid : '';
    const password = typeof payload.password === 'string' ? payload.password : '';
    if (!ssid || /[\s\r\n]/u.test(ssid) || ssid.length > 32 || password.length > 64 || /[\r\n]/u.test(password)) {
      throw new Error('SSID atau password tidak valid');
    }
    body = `CONNECT_WIFI ${ssid} ${password}`;
  }
  const normalizedBody = body.toUpperCase();
  if (normalizedBody !== 'STATUS' && normalizedBody !== 'GET_STATUS' && normalizedBody !== 'SCAN' &&
      normalizedBody !== 'SCAN_WIFI' && normalizedBody !== 'PING' && normalizedBody !== 'LIST_KNOWN_WIFI' &&
      normalizedBody !== 'RETRY_WIFI' && !normalizedBody.startsWith('CONNECT ') &&
      !normalizedBody.startsWith('CONNECT_WIFI ') && !normalizedBody.startsWith('CAL ')) {
    throw new Error('BLE command tidak dikenal');
  }
  if (body.startsWith('CAL ') && (body.length > 135 || /[\r\n]/u.test(body))) {
    throw new Error('Perintah kalibrasi tidak valid');
  }
  const encoded = new TextEncoder().encode(`${body}\n`);
  if (encoded.byteLength > BLE_FRAME_MAX_BYTES) throw new Error('BLE command is too large');
  return encoded;
}

function calibrationToken(value: string): string {
  if (!value || value.length > 96 || /[\s\r\n|]/u.test(value)) {
    throw new Error('Parameter kalibrasi tidak valid');
  }
  return value;
}

export function encodeCalibrationCommand(
  operation: BleCalibrationOperation,
  sensor?: string,
  parameters: Record<string, string | number | boolean> = {},
): Uint8Array {
  if (!BLE_CALIBRATION_OPERATIONS.includes(operation)) throw new Error('Operasi kalibrasi tidak valid');
  const tokens = [`CAL`, operation];
  if (sensor !== undefined) tokens.push(calibrationToken(sensor));
  if (operation === 'CAPTURE') {
    const anchor = parameters.anchor;
    if (typeof anchor !== 'string' || !['tare', 'reference', 'empty', 'full'].includes(anchor)) {
      throw new Error('Anchor kalibrasi wajib dipilih');
    }
    tokens.push(calibrationToken(anchor));
    return encodeBleCommand('', tokens.join(' '));
  }
  if (operation === 'VALIDATE' || operation === 'PREVIEW' || operation === 'COMMIT') {
    return encodeBleCommand('', tokens.join(' '));
  }
  Object.keys(parameters).sort().forEach((key) => {
    const safeKey = calibrationToken(key);
    const value = calibrationToken(String(parameters[key]));
    tokens.push(`${safeKey}=${value}`);
  });
  return encodeBleCommand('', tokens.join(' '));
}

export function decodeBleResponse(bytes: Uint8Array): BleResponse {
  if (bytes.byteLength > BLE_FRAME_MAX_BYTES) throw new Error('BLE response is too large');
  const text = new TextDecoder().decode(bytes).trim();
  if (!text) throw new Error('BLE response is empty');
  if (text.startsWith('ERR ')) return { ok: false, error: text.slice(4) };
  if (text === 'OK SCAN PENDING') return { ok: true, scanPending: true };
  if (text === 'OK PONG') return { ok: true };
  if (text === 'OK CONNECTING') return { ok: true, status: { state: 'CONNECTING', ssid: null } };
  const status = /^OK STATUS state=(\S+) ssid=(.*)$/u.exec(text);
  if (status) {
    const state = status[1] as WifiStatus['state'];
    if (!['SETUP_REQUIRED', 'CONNECTING', 'CONNECTED', 'BACKOFF', 'UNCONFIGURED'].includes(state)) throw new Error('BLE state invalid');
    return { ok: true, status: { state, ssid: status[2] || null } };
  }
  if (text.startsWith('OK SCAN READY')) {
    const lines = text.split('\n').slice(1);
    const networks = lines.map((line) => {
      const match = /^(.*),(-?\d+),(0|1)$/u.exec(line);
      if (!match || !match[1]) throw new Error('BLE scan entry invalid');
      return { ssid: match[1], rssi: Number(match[2]), secure: match[3] === '1' };
    });
    return { ok: true, networks };
  }
  if (text.startsWith('OK LIST_KNOWN_WIFI')) {
    const lines = text.split('\n').slice(1);
    const networks = lines.map((line) => {
      const match = /^(.*),(0|1)$/u.exec(line);
      if (!match || !match[1]) throw new Error('BLE known network entry invalid');
      return { ssid: match[1], rssi: 0, secure: match[2] === '1' };
    });
    return { ok: true, networks };
  }
  if (text === 'OK RETRY_WIFI') return { ok: true, status: { state: 'CONNECTING', ssid: null } };
  if (text.startsWith('CAL1|')) {
    const fields = text.split('|');
    if (fields.length < 6 || fields[0] !== 'CAL1') throw new Error('BLE calibration envelope invalid');
    const values = new Map<string, string>();
    for (const field of fields.slice(1, 5)) {
      const separator = field.indexOf('=');
      if (separator <= 0) throw new Error('BLE calibration field invalid');
      values.set(field.slice(0, separator), field.slice(separator + 1));
    }
    if (values.size !== 4 || !values.has('op') || !values.has('status') ||
        !values.has('session') || !values.has('sensor')) {
      throw new Error('BLE calibration envelope fields invalid');
    }
    const operation = values.get('op') as BleCalibrationResponse['operation'];
    if (![...BLE_CALIBRATION_OPERATIONS, 'PROFILE'].includes(operation as BleCalibrationOperation | 'PROFILE')) {
      throw new Error('BLE calibration operation invalid');
    }
    const status = values.get('status');
    if (status !== 'ACCEPTED' && status !== 'ERROR') throw new Error('BLE calibration status invalid');
    if (!/^\d{1,20}$/u.test(values.get('session') ?? '')) throw new Error('BLE calibration session invalid');
    const dataField = fields.slice(5).join('|');
    if (!dataField.startsWith('data=')) throw new Error('BLE calibration data missing');
    const data: Record<string, string> = {};
    const encoded = dataField.slice(5);
    if (encoded.length > BLE_FRAME_MAX_BYTES) throw new Error('BLE calibration data too large');
    if (encoded) {
      for (const item of encoded.split(';')) {
        if (!item) continue;
        const separator = item.indexOf('=');
        if (separator <= 0) throw new Error('BLE calibration data invalid');
        const key = item.slice(0, separator);
        if (!/^[a-z][a-z0-9_]*$/u.test(key) || key in data) throw new Error('BLE calibration data key invalid');
        data[key] = item.slice(separator + 1);
      }
    }
    const result: BleCalibrationResponse = {
      version: 'CAL1', operation, status, sessionId: values.get('session') ?? '',
      sensor: values.get('sensor') || null, data,
    };
    if (status === 'ERROR') result.error = data.error ?? 'CALIBRATION_REJECTED';
    return status === 'ACCEPTED' ? { ok: true, calibration: result } : { ok: false, error: result.error, calibration: result };
  }
  throw new Error('BLE response envelope is invalid');
}

export function isWebBluetoothAvailable(): boolean {
  return typeof globalThis !== 'undefined' &&
    typeof globalThis.isSecureContext === 'boolean' &&
    globalThis.isSecureContext &&
    typeof (globalThis as unknown as { navigator?: { bluetooth?: unknown } }).navigator?.bluetooth !== 'undefined';
}
