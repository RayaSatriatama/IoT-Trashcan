import { decodeDebugResponse } from '../debug/codec';
import { parseMq135GasJson, type Mq135GasResult } from '../calibration/mq135';
import type { DebugResponse } from '../debug/codec';

export const SIGAP_BLE_SERVICE_UUID = '7d3f0001-7d3f-4a53-9a4b-534947415000';
export const SIGAP_BLE_CONTROL_UUID = '7d3f0002-7d3f-4a53-9a4b-534947415000';
export const SIGAP_BLE_EVENTS_UUID = '7d3f0003-7d3f-4a53-9a4b-534947415000';

export const BLE_FRAME_MAX_BYTES = 2048;
export const BLE_ATT_FRAME_BYTES = 20;
const BLE_MAX_CHUNKS = 512;
const BLE_REQUEST_ID_MAX = 0xFFFFFFFF;
export const BLE_REQUEST_FRAME_VERSION = 'F1';
export const BLE_RESPONSE_FRAME_VERSION = 'R1';
export const BLE_CHUNK_ACK = 'OK CHUNK';
const BLE_CHUNK_ACK_BYTES = new TextEncoder().encode(BLE_CHUNK_ACK);
const BLE_SCAN_RESULT_MAX = 16;

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
  deviceId?: string;
  status?: WifiStatus;
  networks?: WifiNetwork[];
  scanPending?: boolean;
  rawCount?: number;
  calibration?: BleCalibrationResponse;
  debug?: DebugResponse;
};

export type BleCalibrationStatus = 'ACCEPTED' | 'ERROR';
export type BleCalibrationResponse = {
  version: 'CAL1';
  operation: BleCalibrationOperation | 'PROFILE';
  status: BleCalibrationStatus;
  sessionId: string;
  sensor: string | null;
  data: Record<string, string>;
  gas?: Mq135GasResult;
  error?: string;
};

export const BLE_CALIBRATION_OPERATIONS = [
  'CAPABILITIES', 'STATUS', 'EVIDENCE', 'START', 'CRITERIA', 'CAPTURE',
  'VALIDATE', 'PREVIEW', 'COMMIT', 'CANCEL', 'HISTORY', 'ROLLBACK', 'ACTIVE_PROFILE',
] as const;
export type BleCalibrationOperation = typeof BLE_CALIBRATION_OPERATIONS[number];
export type { DebugCursor, DebugLevel, DebugMeta, DebugPage, DebugResponse } from '../debug/codec';

function validateFrameHeader(version: string, requestId: string, chunkIndex: number, chunkCount: number): void {
  if (version !== BLE_REQUEST_FRAME_VERSION && version !== BLE_RESPONSE_FRAME_VERSION) throw new Error('BLE frame version invalid');
  if (!/^\d{1,10}$/u.test(requestId) || Number(requestId) > BLE_REQUEST_ID_MAX) {
    throw new Error('BLE request id invalid');
  }
  if (!Number.isInteger(chunkIndex) || !Number.isInteger(chunkCount) || chunkCount < 1 ||
      chunkCount > BLE_MAX_CHUNKS || chunkIndex < 0 || chunkIndex >= chunkCount) {
    throw new Error('BLE chunk bounds invalid');
  }
}

function splitPayload(payload: Uint8Array, requestId: string, version: typeof BLE_REQUEST_FRAME_VERSION | typeof BLE_RESPONSE_FRAME_VERSION): Uint8Array[] {
  let chunkCount = 1;
  for (;;) {
    const chunks: Uint8Array[] = [];
    let offset = 0;
    while (offset < payload.byteLength || chunks.length === 0) {
      if (chunks.length >= BLE_MAX_CHUNKS) throw new Error('BLE chunk bounds invalid');
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
  const separators: number[] = [];
  for (let index = 0; index < bytes.byteLength && separators.length < 4; index += 1) {
    if (bytes[index] === 0x7c) separators.push(index);
  }
  if (separators.length !== 4 || separators[0] === 0 || separators[1] <= separators[0] ||
      separators[2] <= separators[1] || separators[3] <= separators[2]) {
    throw new Error('BLE frame envelope invalid');
  }
  const header = new TextDecoder('utf-8', { fatal: true });
  const version = header.decode(bytes.slice(0, separators[0])) as BleFrame['version'];
  const requestId = header.decode(bytes.slice(separators[0] + 1, separators[1]));
  const chunkIndexToken = header.decode(bytes.slice(separators[1] + 1, separators[2]));
  const chunkCountToken = header.decode(bytes.slice(separators[2] + 1, separators[3]));
  if (!/^\d+$/u.test(chunkIndexToken) || !/^\d+$/u.test(chunkCountToken)) {
    throw new Error('BLE chunk bounds invalid');
  }
  const chunkIndex = Number(chunkIndexToken);
  const chunkCount = Number(chunkCountToken);
  validateFrameHeader(version, requestId, chunkIndex, chunkCount);
  // Keep payload bytes opaque. A frame may end in the middle of UTF-8 codepoint.
  const payload = bytes.slice(separators[3] + 1);
  if (bytes.byteLength > BLE_ATT_FRAME_BYTES) throw new Error('BLE frame size invalid');
  return { version, requestId, chunkIndex, chunkCount, payload };
}

export function isBleChunkAcknowledgement(frame: BleFrame): boolean {
  return frame.version === BLE_RESPONSE_FRAME_VERSION && frame.chunkIndex === 0 && frame.chunkCount === 1 &&
    frame.payload.byteLength === BLE_CHUNK_ACK_BYTES.byteLength &&
    BLE_CHUNK_ACK_BYTES.every((byte, index) => frame.payload[index] === byte);
}

export function assembleBleFrames(
  frames: readonly BleFrame[],
  expectedVersion: BleFrame['version'] = BLE_RESPONSE_FRAME_VERSION,
): Uint8Array {
  if (frames.length === 0) throw new Error('BLE response is empty');
  if (frames.length > BLE_MAX_CHUNKS) throw new Error('BLE chunk bounds invalid');
  const first = frames[0];
  validateFrameHeader(first.version, first.requestId, first.chunkIndex, first.chunkCount);
  if (first.version !== expectedVersion || frames.length !== first.chunkCount) throw new Error('BLE response chunks incomplete');
  const ordered = [...frames].sort((left, right) => left.chunkIndex - right.chunkIndex);
  let outputSize = 0;
  ordered.forEach((frame, index) => {
    validateFrameHeader(frame.version, frame.requestId, frame.chunkIndex, frame.chunkCount);
    if (frame.version !== first.version || frame.requestId !== first.requestId || frame.chunkCount !== first.chunkCount || frame.chunkIndex !== index) {
      throw new Error('BLE response chunks inconsistent');
    }
    outputSize += frame.payload.byteLength;
  });
  if (outputSize > BLE_FRAME_MAX_BYTES) throw new Error('BLE response is too large');
  const output = new Uint8Array(outputSize);
  let offset = 0;
  ordered.forEach((frame) => {
    output.set(frame.payload, offset);
    offset += frame.payload.byteLength;
  });
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
  if (normalizedBody !== 'STATUS' && normalizedBody !== 'GET_STATUS' && normalizedBody !== 'GET_DEVICE_ID' && normalizedBody !== 'SCAN' &&
      normalizedBody !== 'SCAN_WIFI' && normalizedBody !== 'PING' && normalizedBody !== 'LIST_KNOWN_WIFI' &&
      normalizedBody !== 'RETRY_WIFI' && !normalizedBody.startsWith('CONNECT ') &&
      !normalizedBody.startsWith('CONNECT_WIFI ') && !normalizedBody.startsWith('CAL ') &&
      !normalizedBody.startsWith('GET_DEBUG ') && !normalizedBody.startsWith('SET_DEBUG_LEVEL ')) {
    throw new Error('BLE command tidak dikenal');
  }
  // Match the firmware 1024-byte CAL command workspace, including newline.
  if (body.startsWith('CAL ') && (body.length > 1022 || /[\r\n]/u.test(body))) {
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

function validateMq135NormalizedCriteria(parameters: Record<string, string | number | boolean>): void {
  const required = [
    'response_mode','samples','min_duration_ms','max_age_ms','max_variation_milli_percent','circuit_mv',
    'divider_top_ohm','divider_bottom_ohm','topology','hardware_confirmed','reference_confirmed','hardware_identity',
    'reference_environment','reference_kind','preparation_confirmed','preparation_method','heating_duration_us',
  ];
  const allowed = new Set([...required,'reference_temperature_centi_c','reference_humidity_centi_pct']);
  if (required.some(key => !Object.hasOwn(parameters,key)) || Object.keys(parameters).some(key => !allowed.has(key)) ||
      parameters.response_mode !== 'normalized' || !['operational','clean_air_confirmed'].includes(String(parameters.reference_kind)) ||
      !['unknown','operator_declared'].includes(String(parameters.preparation_method)) ||
      typeof parameters.hardware_identity !== 'string' || !/^[A-Za-z0-9_.:-]{1,31}$/u.test(parameters.hardware_identity) ||
      typeof parameters.reference_environment !== 'string' || !/^[A-Za-z0-9_.:-]{1,31}$/u.test(parameters.reference_environment)) {
    throw new Error('MQ135 normalized criteria invalid');
  }
  const integer = (key: string, min: number, max: number) => typeof parameters[key] === 'number' &&
    Number.isSafeInteger(parameters[key]) && parameters[key] >= min && parameters[key] <= max;
  if (!integer('samples',3,64) || !integer('min_duration_ms',1,Number.MAX_SAFE_INTEGER) ||
      !integer('max_age_ms',1,Number.MAX_SAFE_INTEGER) || !integer('max_variation_milli_percent',1,100000) ||
      !integer('circuit_mv',1,0xFFFFFFFF) || !integer('divider_top_ohm',1,0xFFFFFFFF) ||
      !integer('divider_bottom_ohm',1,0xFFFFFFFF) || !integer('topology',1,2) ||
      (parameters.topology !== 1 && parameters.topology !== 2) ||
      !integer('hardware_confirmed',0,1) || !integer('reference_confirmed',0,1) ||
      !integer('preparation_confirmed',0,1) || !integer('heating_duration_us',0,Number.MAX_SAFE_INTEGER) ||
      (Object.hasOwn(parameters,'reference_temperature_centi_c') && !integer('reference_temperature_centi_c',-2147483648,2147483647)) ||
      (Object.hasOwn(parameters,'reference_humidity_centi_pct') && !integer('reference_humidity_centi_pct',0,10000))) {
    throw new Error('MQ135 normalized criteria invalid');
  }
  if (parameters.preparation_confirmed === 1 && parameters.preparation_method !== 'operator_declared') {
    throw new Error('MQ135 preparation requires operator declaration');
  }
  const heatingDuration = parameters.heating_duration_us;
  if (parameters.reference_kind === 'clean_air_confirmed' &&
      (parameters.preparation_confirmed !== 1 || parameters.preparation_method !== 'operator_declared' || typeof heatingDuration !== 'number' || heatingDuration <= 172800000000)) {
    throw new Error('MQ135 clean-air reference requires confirmed preparation and more than 48 hours of heating');
  }
}

export function encodeCalibrationCommand(
  operation: BleCalibrationOperation,
  sensor?: string,
  parameters: Record<string, string | number | boolean> = {},
): Uint8Array {
  if (!BLE_CALIBRATION_OPERATIONS.includes(operation)) throw new Error('Operasi kalibrasi tidak valid');
  const operationNeedsSensor = !['CAPABILITIES', 'STATUS', 'HISTORY', 'CANCEL'].includes(operation);
  if (operationNeedsSensor && sensor === undefined) throw new Error('Sensor kalibrasi wajib dipilih');
  const tokens = [`CAL`, operation];
  if (sensor !== undefined) {
    calibrationToken(sensor);
    if (!/^(?:hx711_(?:organic|inorganic|b3)|ultrasonic_(?:organic|inorganic|b3)|mq(?:4|135(?:_gas)?|7|8)|ds18b20_b3|sht31_(?:44|45))$/u.test(sensor)) {
      throw new Error('Sensor kalibrasi tidak dikenal');
    }
    tokens.push(sensor);
  }
  if (operation === 'CRITERIA' && sensor === 'mq135_gas') validateMq135NormalizedCriteria(parameters);
  if (operation === 'CAPTURE') {
    const anchor = parameters.anchor;
    if (typeof anchor !== 'string' || !['tare', 'reference', 'empty', 'full'].includes(anchor)) {
      throw new Error('Anchor kalibrasi wajib dipilih');
    }
    tokens.push(calibrationToken(anchor));
    return encodeBleCommand('', tokens.join(' '));
  }
  if (operation === 'VALIDATE' || operation === 'PREVIEW' || operation === 'CANCEL') {
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
  const rawText = new TextDecoder('utf-8', { ignoreBOM: true }).decode(bytes);
  if (rawText.startsWith('DBG1|')) return { ok: true, debug: decodeDebugResponse(rawText) };
  const text = rawText.trim();
  if (!text) throw new Error('BLE response is empty');
  if (text.startsWith('ERR ')) return { ok: false, error: text.slice(4) };
  if (text === 'OK SCAN PENDING') return { ok: true, scanPending: true };
  if (text === 'OK PONG') return { ok: true };
  const deviceIdentity = /^OK DEVICE_ID id=([0-9a-f]{16})$/u.exec(text);
  if (deviceIdentity) return { ok: true, deviceId: deviceIdentity[1] };
  if (text === 'OK CONNECTING') return { ok: true, status: { state: 'CONNECTING', ssid: null } };
  const status = /^OK STATUS state=(\S+) ssid=(.*)$/u.exec(text);
  if (status) {
    const state = status[1] as WifiStatus['state'];
    if (!['SETUP_REQUIRED', 'CONNECTING', 'CONNECTED', 'BACKOFF', 'UNCONFIGURED'].includes(state)) throw new Error('BLE state invalid');
    return { ok: true, status: { state, ssid: status[2] || null } };
  }
  if (text.startsWith('OK SCAN READY')) {
    const parts = text.split('\n');
    const header = /^OK SCAN READY(?: count=(\d{1,2}))?$/u.exec(parts[0]);
    if (!header) throw new Error('BLE scan header invalid');
    const lines = parts.slice(1);
    const declaredCount = header[1] === undefined ? null : Number(header[1]);
    if (declaredCount !== null && (declaredCount > BLE_SCAN_RESULT_MAX || declaredCount !== lines.length)) {
      throw new Error('BLE scan count invalid');
    }
    if (lines.length > BLE_SCAN_RESULT_MAX) throw new Error('BLE scan result limit exceeded');
    const rows = lines.map((line) => {
      const match = /^(.*),(-?\d+),(0|1)$/u.exec(line);
      if (!match) throw new Error('BLE scan entry invalid');
      const rssi = Number(match[2]);
      if (!Number.isFinite(rssi)) throw new Error('BLE scan entry invalid');
      return { ssid: match[1], rssi, secure: match[3] === '1' };
    });
    const networks = rows.filter((network) => network.ssid.length > 0);
    return { ok: true, networks, rawCount: rows.length };
  }
  if (text.startsWith('OK LIST_KNOWN_WIFI')) {
    const parts = text.split('\n');
    const header = /^OK LIST_KNOWN_WIFI(?: count=(\d{1,2}))?$/u.exec(parts[0]);
    if (!header) throw new Error('BLE known Wi-Fi header invalid');
    const lines = parts.slice(1);
    const declaredCount = header[1] === undefined ? null : Number(header[1]);
    if (declaredCount !== null && (declaredCount > BLE_SCAN_RESULT_MAX || declaredCount !== lines.length)) {
      throw new Error('BLE known Wi-Fi count invalid');
    }
    if (lines.length > BLE_SCAN_RESULT_MAX) throw new Error('BLE known Wi-Fi result limit exceeded');
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
    let gas: Mq135GasResult | undefined;
    if (values.get('sensor') === 'mq135_gas' && encoded.startsWith('{')) {
      const parsed = parseMq135GasJson(encoded);
      if (!parsed) throw new Error('MQ135 result invalid');
      gas = parsed;
    } else if (encoded) {
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
      sensor: values.get('sensor') || null, data, ...(gas ? { gas } : {}),
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
