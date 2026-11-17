import {
  assembleBleFrames,
  decodeBleFrame,
  decodeBleResponse,
  encodeBleFrames,
  isBleChunkAcknowledgement,
  encodeBleCommand,
  encodeCalibrationCommand,
  isWebBluetoothAvailable,
  SIGAP_BLE_CONTROL_UUID,
  SIGAP_BLE_EVENTS_UUID,
  SIGAP_BLE_SERVICE_UUID,
  type WifiNetwork,
  type WifiStatus,
  type BleCalibrationOperation,
  type BleCalibrationResponse,
  type DebugCursor,
  type DebugLevel,
  type DebugMeta,
  type DebugPage,
} from './protocol';

type BleCharacteristic = {
  writeValue: (value: BufferSource) => Promise<void>;
  startNotifications: () => Promise<BleCharacteristic>;
  addEventListener: (event: string, listener: (event: { target?: { value?: DataView } }) => void) => void;
};
type BleService = { getCharacteristic: (uuid: string) => Promise<BleCharacteristic> };
type BleServer = { getPrimaryService: (uuid: string) => Promise<BleService>; disconnect?: () => void };
type BleDevice = { name?: string; gatt?: { connect: () => Promise<BleServer> }; addEventListener: (event: string, listener: () => void) => void };
type BluetoothRequestOptions = {
  filters: Array<{ namePrefix?: string }>;
  optionalServices: string[];
};
type BluetoothAdapter = { requestDevice: (options: BluetoothRequestOptions) => Promise<BleDevice> };

export type BleProvisioningClientOptions = {
  requestTimeoutMs?: number;
  readOnlyMaxAttempts?: number;
  commandTimeoutMs?: Partial<Record<'GET_DEVICE_ID' | 'GET_STATUS' | 'SCAN_WIFI' | 'GET_DEBUG', number>>;
  connectTimeoutMs?: number;
  connectPollIntervalMs?: number;
  sleep?: (milliseconds: number) => Promise<void>;
  onConnectionChange?: (connected: boolean) => void;
  // Device name is display-only and is never used as an identity proof.
  onDeviceIdentityChange?: (name: string | null) => void;
  onVerifiedDeviceIdentityChange?: (deviceId: string | null) => void;
  onDiagnosticEvent?: (event: BleClientDiagnosticEvent) => void;
};

export type BleClientDiagnosticEvent = {
  type: 'request_started' | 'write_complete' | 'response_fragment' | 'frame_ignored' |
    'scan_pending' | 'decoder_rejected' | 'request_complete' | 'request_timeout' | 'session_disconnected';
  operation?: 'GET_DEVICE_ID' | 'GET_STATUS' | 'SCAN_WIFI' | 'GET_DEBUG' | 'LIST_KNOWN_WIFI' | 'RETRY_WIFI' | 'CONNECT_WIFI' | 'CALIBRATION' | 'OTHER';
  requestId?: string;
  elapsedMs?: number;
  chunkIndex?: number;
  expectedCount?: number;
  receivedCount?: number;
  byteLength?: number;
  reason?: 'request_id_mismatch' | 'version_mismatch' | 'duplicate_fragment' | 'chunk_acknowledgement';
  category?: 'scan_header_invalid' | 'scan_count_invalid' | 'scan_entry_invalid' |
    'scan_result_limit_exceeded' | 'response_envelope_invalid' | 'other_decoder_rejection';
  outcome?: 'accepted' | 'device_rejected';
  lastPhase?: 'request_started' | 'write_complete' | 'response_fragment' | 'scan_pending' | 'decoder_rejected';
  attempt?: number;
  maxAttempts?: number;
};

class ReadTimeoutError extends Error {}

type DiagnosticOperation = NonNullable<BleClientDiagnosticEvent['operation']>;
const diagnosticOperation = (command: string): DiagnosticOperation => {
  const operation = command.split(' ', 1)[0];
  switch (operation) {
    case 'GET_DEVICE_ID': case 'GET_STATUS': case 'SCAN_WIFI': case 'GET_DEBUG': case 'LIST_KNOWN_WIFI':
    case 'RETRY_WIFI': case 'CONNECT_WIFI': return operation;
    default: return 'OTHER';
  }
};

const decoderCategory = (error: unknown, operation: DiagnosticOperation): NonNullable<BleClientDiagnosticEvent['category']> => {
  if (operation === 'SCAN_WIFI' && error instanceof Error) {
    switch (error.message) {
      case 'BLE scan header invalid': return 'scan_header_invalid';
      case 'BLE scan count invalid': return 'scan_count_invalid';
      case 'BLE scan entry invalid': return 'scan_entry_invalid';
      case 'BLE scan result limit exceeded': return 'scan_result_limit_exceeded';
    }
  }
  return error instanceof Error && error.message === 'BLE response envelope is invalid'
    ? 'response_envelope_invalid' : 'other_decoder_rejection';
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
    operation: DiagnosticOperation;
    startedAt: number;
    lastPhase: NonNullable<BleClientDiagnosticEvent['lastPhase']>;
    expectedCount: number | null;
    writesComplete: boolean;
    decoderRejected: boolean;
  } | null = null;
  private nextRequestId = 1;
  private readonly requestTimeoutMs: number;
  private readonly commandTimeoutMs: BleProvisioningClientOptions['commandTimeoutMs'];
  private readonly readOnlyMaxAttempts: number;
  private readonly connectTimeoutMs: number;
  private readonly connectPollIntervalMs: number;
  private readonly sleep: (milliseconds: number) => Promise<void>;
  private readonly onConnectionChange?: (connected: boolean) => void;
  private readonly onDeviceIdentityChange?: (name: string | null) => void;
  private readonly onVerifiedDeviceIdentityChange?: (deviceId: string | null) => void;
  private readonly onDiagnosticEvent?: (event: BleClientDiagnosticEvent) => void;
  private verifiedDeviceIdValue: string | null = null;
  private sessionGeneration = 0;

  constructor(options: BleProvisioningClientOptions = {}) {
    this.requestTimeoutMs = options.requestTimeoutMs ?? 5000;
    this.commandTimeoutMs = {};
    for (const [command, timeout] of Object.entries(options.commandTimeoutMs ?? {})) {
      if (!['GET_DEVICE_ID', 'GET_STATUS', 'SCAN_WIFI', 'GET_DEBUG'].includes(command) ||
          typeof timeout !== 'number' || !Number.isFinite(timeout) || timeout <= 0) {
        throw new Error('BLE command timeout invalid');
      }
      (this.commandTimeoutMs as Record<string, number>)[command] = timeout;
    }
    const attempts = options.readOnlyMaxAttempts ?? 1;
    this.readOnlyMaxAttempts = Number.isFinite(attempts) ? Math.min(3, Math.max(1, Math.floor(attempts))) : 1;
    this.connectTimeoutMs = options.connectTimeoutMs ?? 12000;
    this.connectPollIntervalMs = options.connectPollIntervalMs ?? 500;
    this.sleep = options.sleep ?? ((milliseconds) => new Promise((resolve) => globalThis.setTimeout(resolve, milliseconds)));
    this.onConnectionChange = options.onConnectionChange;
    this.onDeviceIdentityChange = options.onDeviceIdentityChange;
    this.onVerifiedDeviceIdentityChange = options.onVerifiedDeviceIdentityChange;
    this.onDiagnosticEvent = options.onDiagnosticEvent;
  }

  private diagnostic(event: BleClientDiagnosticEvent): void {
    try { this.onDiagnosticEvent?.(event); } catch { /* Diagnostics must not affect BLE requests. */ }
  }

  get verifiedDeviceId(): string | null {
    return this.verifiedDeviceIdValue;
  }

  async connect(): Promise<void> {
    if (this.device || this.server) this.close();
    const generation = ++this.sessionGeneration;
    let server: BleServer | null = null;
    try {
      const device = await bluetooth().requestDevice({
        filters: [{ namePrefix: 'SIGAP-' }],
        optionalServices: [SIGAP_BLE_SERVICE_UUID],
      });
      this.requireCurrentSession(generation);
      if (!device.gatt) throw new Error('Perangkat BLE tidak menyediakan GATT');
      server = await device.gatt.connect();
      this.requireCurrentSession(generation);
      const service = await server.getPrimaryService(SIGAP_BLE_SERVICE_UUID);
      this.requireCurrentSession(generation);
      const control = await service.getCharacteristic(SIGAP_BLE_CONTROL_UUID);
      this.requireCurrentSession(generation);
      const events = await service.getCharacteristic(SIGAP_BLE_EVENTS_UUID);
      this.requireCurrentSession(generation);
      await events.startNotifications();
      this.requireCurrentSession(generation);
      events.addEventListener('characteristicvaluechanged', (event) => {
        if (generation !== this.sessionGeneration || this.device !== device) return;
        const value = event.target?.value;
        if (!value) return;
        try {
          const frame = decodeBleFrame(new Uint8Array(value.buffer, value.byteOffset, value.byteLength));
          const waiter = this.pending;
          if (!waiter) return;
          const elapsedMs = Date.now() - waiter.startedAt;
          if (frame.requestId !== waiter.requestId) {
            this.diagnostic({ type: 'frame_ignored', operation: waiter.operation, requestId: waiter.requestId, elapsedMs, reason: 'request_id_mismatch' });
            return;
          }
          if (frame.version !== 'R1') {
            this.diagnostic({ type: 'frame_ignored', operation: waiter.operation, requestId: waiter.requestId, elapsedMs, reason: 'version_mismatch' });
            return;
          }
          if (isBleChunkAcknowledgement(frame)) {
            this.diagnostic({ type: 'frame_ignored', operation: waiter.operation, requestId: waiter.requestId, elapsedMs, reason: 'chunk_acknowledgement' });
            return;
          }
          if (waiter.frames.some((candidate) => candidate.chunkIndex === frame.chunkIndex)) {
            this.diagnostic({ type: 'frame_ignored', operation: waiter.operation, requestId: waiter.requestId, elapsedMs, reason: 'duplicate_fragment' });
            return;
          }
          waiter.frames.push(frame);
          waiter.expectedCount = frame.chunkCount;
          waiter.lastPhase = 'response_fragment';
          this.diagnostic({ type: 'response_fragment', operation: waiter.operation, requestId: waiter.requestId,
            elapsedMs, chunkIndex: frame.chunkIndex, expectedCount: frame.chunkCount,
            receivedCount: waiter.frames.length, byteLength: frame.payload.byteLength });
          if (waiter.frames.length !== frame.chunkCount) return;
          let response: ReturnType<typeof decodeBleResponse>;
          try {
            response = decodeBleResponse(assembleBleFrames(waiter.frames));
          } catch (error) {
            waiter.decoderRejected = true;
            waiter.lastPhase = 'decoder_rejected';
            this.diagnostic({ type: 'decoder_rejected', operation: waiter.operation, requestId: waiter.requestId,
              elapsedMs: Date.now() - waiter.startedAt, category: decoderCategory(error, waiter.operation) });
            return;
          }
          if (!waiter.accept(response)) {
            waiter.frames.length = 0;
            waiter.lastPhase = 'scan_pending';
            this.diagnostic({ type: 'scan_pending', operation: waiter.operation, requestId: waiter.requestId,
              elapsedMs: Date.now() - waiter.startedAt });
            return;
          }
          this.pending = null;
          this.diagnostic({ type: 'request_complete', operation: waiter.operation, requestId: waiter.requestId,
            elapsedMs: Date.now() - waiter.startedAt, outcome: response.ok ? 'accepted' : 'device_rejected' });
          if (response.ok) waiter.resolve(response);
          else waiter.reject(new Error(response.error || 'Perangkat menolak permintaan'));
        } catch {
          // Invalid frames are ignored; the request timeout provides bounded recovery.
        }
      });
      this.requireCurrentSession(generation);
      this.server = server;
      this.control = control;
      this.device = device;
      device.addEventListener('gattserverdisconnected', () => {
        if (generation === this.sessionGeneration && this.device === device) this.close();
      });
      this.onDeviceIdentityChange?.(device.name ?? null);
      this.verifiedDeviceIdValue = null;
      this.onVerifiedDeviceIdentityChange?.(null);
      this.onConnectionChange?.(true);
    } catch (error) {
      server?.disconnect?.();
      throw error;
    }
  }

  /**
   * Compare device-reported identity over ordinary GATT. This is a routing
   * check, not proof of operator authentication.
   */
  async verifyDeviceIdentity(expectedDeviceId: string): Promise<string> {
    const expected = expectedDeviceId.trim().toLowerCase();
    if (!/^[0-9a-f]{16}$/u.test(expected)) {
      throw new Error('Identitas perangkat SIGAP tidak canonical');
    }
    const generation = this.sessionGeneration;
    const response = await this.request('GET_DEVICE_ID');
    this.requireCurrentSession(generation);
    if (!response.deviceId || response.deviceId !== expected) {
      this.verifiedDeviceIdValue = null;
      this.onVerifiedDeviceIdentityChange?.(null);
      throw new Error('Perangkat BLE tidak cocok dengan device_id yang dipilih');
    }
    this.verifiedDeviceIdValue = response.deviceId;
    this.onVerifiedDeviceIdentityChange?.(response.deviceId);
    return response.deviceId;
  }

  private requireCurrentSession(generation: number): void {
    if (generation !== this.sessionGeneration) throw new Error('BLE terputus');
  }

  close(): void {
    if (this.device || this.server) this.diagnostic({ type: 'session_disconnected' });
    this.sessionGeneration += 1;
    this.pending?.reject(new Error('BLE terputus'));
    this.pending = null;
    this.control = null;
    this.server?.disconnect?.();
    this.server = null;
    this.device = null;
    this.verifiedDeviceIdValue = null;
    this.onVerifiedDeviceIdentityChange?.(null);
    this.onDeviceIdentityChange?.(null);
    this.onConnectionChange?.(false);
  }

  async debugMeta(): Promise<DebugMeta> {
    this.requireVerifiedDeviceIdentity();
    const generation = this.sessionGeneration;
    const response = await this.request('GET_DEBUG META');
    this.requireCurrentSession(generation);
    if (response.debug?.kind !== 'meta') throw new Error('Respons debug META tidak sesuai');
    return response.debug.meta;
  }

  async debugPage(cursor: DebugCursor): Promise<DebugPage> {
    this.requireVerifiedDeviceIdentity();
    if (!cursor || typeof cursor.boot_id !== 'string' || typeof cursor.after !== 'string' ||
        !/^[0-9a-f]{32}$/u.test(cursor.boot_id) || !/^(0|[1-9][0-9]{0,19})$/u.test(cursor.after) ||
        BigInt(cursor.after) > 18446744073709551615n) {
      throw new Error('Cursor debug tidak valid');
    }
    const generation = this.sessionGeneration;
    const response = await this.request(`GET_DEBUG PAGE ${cursor.boot_id} ${cursor.after}`);
    this.requireCurrentSession(generation);
    if (response.debug?.kind !== 'page') throw new Error('Respons debug PAGE tidak sesuai');
    return response.debug.page;
  }

  async setDebugLevel(level: DebugLevel): Promise<DebugLevel> {
    this.requireVerifiedDeviceIdentity();
    if (!['ERROR', 'WARN', 'INFO', 'DEBUG'].includes(level)) throw new Error('Level debug tidak valid');
    const generation = this.sessionGeneration;
    const response = await this.request(`SET_DEBUG_LEVEL ${level}`);
    this.requireCurrentSession(generation);
    if (response.debug?.kind !== 'level') throw new Error('Respons debug LEVEL tidak sesuai');
    return response.debug.level;
  }

  async status(): Promise<WifiStatus> {
    this.requireVerifiedDeviceIdentity();
    const generation = this.sessionGeneration;
    const response = await this.request('GET_STATUS');
    this.requireCurrentSession(generation);
    if (!response.status) throw new Error('Status WiFi tidak tersedia');
    return response.status;
  }

  async scanDetailed(): Promise<{ networks: WifiNetwork[]; rawCount: number }> {
    this.requireVerifiedDeviceIdentity();
    const generation = this.sessionGeneration;
    const response = await this.request('SCAN_WIFI', {}, (value) => !value.scanPending);
    this.requireCurrentSession(generation);
    return { networks: response.networks ?? [], rawCount: response.rawCount ?? response.networks?.length ?? 0 };
  }

  async scan(): Promise<WifiNetwork[]> {
    return (await this.scanDetailed()).networks;
  }

  async listKnownWifi(): Promise<WifiNetwork[]> {
    this.requireVerifiedDeviceIdentity();
    const generation = this.sessionGeneration;
    const response = await this.request('LIST_KNOWN_WIFI');
    this.requireCurrentSession(generation);
    return response.networks ?? [];
  }

  async retryWifi(): Promise<WifiStatus> {
    this.requireVerifiedDeviceIdentity();
    const generation = this.sessionGeneration;
    const response = await this.request('RETRY_WIFI');
    this.requireCurrentSession(generation);
    if (!response.status) throw new Error('Status retry Wi-Fi tidak tersedia');
    return response.status;
  }

  async connectWifi(ssid: string, password: string): Promise<WifiStatus> {
    this.requireVerifiedDeviceIdentity();
    const generation = this.sessionGeneration;
    if (!ssid.trim() || ssid.length > 32 || password.length > 64) throw new Error('SSID atau password tidak valid');
    const response = await this.request('CONNECT_WIFI', { ssid, password });
    this.requireCurrentSession(generation);
    if (!response.status) throw new Error('Status koneksi tidak tersedia');
    if (response.status.state === 'CONNECTED') return response.status;
    if (response.status.state === 'SETUP_REQUIRED' || response.status.state === 'UNCONFIGURED') {
      throw new Error('Perangkat menolak kredensial Wi-Fi');
    }
    const deadline = Date.now() + this.connectTimeoutMs;
    let latest = response.status;
    while (Date.now() < deadline) {
      await this.sleep(this.connectPollIntervalMs);
      this.requireCurrentSession(generation);
      latest = await this.status();
      this.requireCurrentSession(generation);
      if (latest.state === 'CONNECTED') return latest;
      if (latest.state === 'SETUP_REQUIRED' || latest.state === 'UNCONFIGURED') {
        throw new Error('Perangkat gagal menyimpan kredensial Wi-Fi');
      }
    }
    this.requireCurrentSession(generation);
    throw new Error(`Koneksi Wi-Fi tidak selesai (status ${latest.state})`);
  }

  private requireVerifiedDeviceIdentity(): void {
    if (this.control === null) {
      throw new Error('Hubungkan perangkat BLE terlebih dahulu');
    }
    if (this.verifiedDeviceIdValue === null) {
      throw new Error('Identitas perangkat BLE belum diverifikasi');
    }
  }

  async calibrationCapabilities(sensor: string): Promise<BleCalibrationResponse> {
    return this.calibration('CAPABILITIES', sensor);
  }

  async calibrationStatus(sensor: string): Promise<BleCalibrationResponse> {
    return this.calibration('STATUS', sensor);
  }

  async calibrationEvidence(sensor: string): Promise<BleCalibrationResponse> {
    return this.calibration('EVIDENCE', sensor);
  }

  async calibrationStart(
    sensor: string,
    parameters: Record<string, string | number | boolean> = {},
  ): Promise<BleCalibrationResponse> {
    return this.calibration('START', sensor, parameters);
  }

  async calibrationCriteria(
    sensor: string,
    parameters: Record<string, string | number | boolean>,
  ): Promise<BleCalibrationResponse> {
    return this.calibration('CRITERIA', sensor, parameters);
  }

  async calibrationCapture(sensor: string, anchor: 'tare' | 'reference' | 'empty' | 'full'): Promise<BleCalibrationResponse> {
    return this.calibration('CAPTURE', sensor, { anchor });
  }

  async calibrationValidate(sensor: string): Promise<BleCalibrationResponse> {
    return this.calibration('VALIDATE', sensor);
  }

  async calibrationPreview(sensor: string): Promise<BleCalibrationResponse> {
    return this.calibration('PREVIEW', sensor);
  }

  async calibrationCommit(sensor: string, expected: Record<string, string | number | boolean> = {}): Promise<BleCalibrationResponse> {
    return this.calibration('COMMIT', sensor, expected);
  }

  async calibrationCancel(): Promise<BleCalibrationResponse> {
    return this.calibration('CANCEL');
  }

  async calibrationHistory(sensor?: string): Promise<BleCalibrationResponse> {
    return this.calibration('HISTORY', sensor);
  }

  async calibrationRollback(sensor: string, version: string, expectedHash?: string): Promise<BleCalibrationResponse> {
    if (sensor === 'mq135_gas') {
      if (!expectedHash) throw new Error('Identitas profil aktif wajib tersedia.');
      return this.calibration('ROLLBACK',sensor,{expected_profile_version:version,expected_profile_hash:expectedHash});
    }
    return this.calibration('ROLLBACK', sensor, { version });
  }

  private async calibration(
    operation: BleCalibrationOperation,
    sensor?: string,
    parameters: Record<string, string | number | boolean> = {},
  ): Promise<BleCalibrationResponse> {
    if (operation === 'START' || operation === 'CRITERIA' || operation === 'CAPTURE' ||
        operation === 'VALIDATE' || operation === 'PREVIEW' || operation === 'COMMIT' ||
        operation === 'CANCEL' || operation === 'ROLLBACK') {
      if (this.verifiedDeviceIdValue === null) {
        throw new Error('Identitas perangkat BLE belum diverifikasi');
      }
    }
    const response = await this.requestCalibration(
      encodeCalibrationCommand(operation, sensor, parameters),
    );
    return response;
  }

  private async requestCalibration(command: Uint8Array): Promise<BleCalibrationResponse> {
    if (this.pending) throw new Error('Permintaan BLE sebelumnya masih berjalan');
    const control = this.control;
    if (!control) throw new Error('Hubungkan perangkat BLE terlebih dahulu');
    const requestId = String(this.nextRequestId++);
    const frames = encodeBleFrames(requestId, command.subarray(0, command.length - 1));
    const attempt = 1;
    const maxAttempts = 1;
    const startedAt = Date.now();
    this.diagnostic({ type: 'request_started', operation: 'CALIBRATION', requestId, elapsedMs: 0 });
    const response = await new Promise<ReturnType<typeof decodeBleResponse>>((resolve, reject) => {
      const timer = globalThis.setTimeout(() => {
        const waiter = this.pending?.requestId === requestId ? this.pending : null;
        if (waiter) this.diagnostic({ type: 'request_timeout', operation: waiter.operation, requestId,
          elapsedMs: Date.now() - startedAt, expectedCount: waiter.expectedCount ?? undefined,
          receivedCount: waiter.frames.length, lastPhase: waiter.lastPhase, attempt, maxAttempts });
        if (this.pending?.requestId === requestId) this.pending = null;
        reject(waiter?.writesComplete && !waiter.decoderRejected
          ? new ReadTimeoutError('Perangkat tidak merespons') : new Error('Perangkat tidak merespons'));
      }, this.requestTimeoutMs);
      this.pending = {
        requestId,
        frames: [],
        accept: () => true,
        operation: 'CALIBRATION',
        startedAt,
        lastPhase: 'request_started',
        expectedCount: null,
        writesComplete: false,
        decoderRejected: false,
        resolve: (value) => { globalThis.clearTimeout(timer); resolve(value); },
        reject: (error) => { globalThis.clearTimeout(timer); reject(error); },
      };
      void (async () => {
        try {
          for (const frame of frames) {
            if (this.pending?.requestId !== requestId) return;
            await control.writeValue(frame as unknown as BufferSource);
          }
          if (this.pending?.requestId === requestId) this.pending.writesComplete = true;
        } catch (error: unknown) {
          globalThis.clearTimeout(timer);
          if (this.pending?.requestId === requestId) this.pending = null;
          reject(error instanceof Error ? error : new Error('BLE write gagal'));
        }
      })();
    });
    if (!response.calibration || !response.calibration.status) throw new Error('Respons kalibrasi tidak tersedia');
    return response.calibration;
  }

  private async request(command: string, payload: Record<string, unknown> = {}, accept: (response: ReturnType<typeof decodeBleResponse>) => boolean = () => true): Promise<ReturnType<typeof decodeBleResponse>> {
    const operation = diagnosticOperation(command);
    const maxAttempts = ['GET_DEVICE_ID', 'GET_STATUS', 'SCAN_WIFI'].includes(operation) ? this.readOnlyMaxAttempts : 1;
    const generation = this.sessionGeneration;
    for (let attempt = 1; ; attempt += 1) {
      try {
        const response = await this.requestAttempt(command, payload, accept, attempt, maxAttempts);
        this.requireCurrentSession(generation);
        return response;
      } catch (error) {
        if (!(error instanceof ReadTimeoutError) || attempt >= maxAttempts) throw error;
        this.requireCurrentSession(generation);
      }
    }
  }

  private async requestAttempt(command: string, payload: Record<string, unknown>, accept: (response: ReturnType<typeof decodeBleResponse>) => boolean, attempt: number, maxAttempts: number): Promise<ReturnType<typeof decodeBleResponse>> {
    if (this.pending) throw new Error('Permintaan BLE sebelumnya masih berjalan');
    const control = this.control;
    if (!control) throw new Error('Hubungkan perangkat BLE terlebih dahulu');
    const value = encodeBleCommand('', command, payload);
    const requestId = String(this.nextRequestId++);
    const frames = encodeBleFrames(requestId, value.subarray(0, value.length - 1));
    const operation = diagnosticOperation(command);
    const startedAt = Date.now();
    this.diagnostic({ type: 'request_started', operation, requestId, elapsedMs: 0, attempt, maxAttempts });
    const timeoutMs = operation === 'GET_DEVICE_ID' || operation === 'GET_STATUS' ||
      operation === 'SCAN_WIFI' || operation === 'GET_DEBUG'
      ? this.commandTimeoutMs?.[operation] ?? this.requestTimeoutMs : this.requestTimeoutMs;
    return new Promise((resolve, reject) => {
      const timer = globalThis.setTimeout(() => {
        const waiter = this.pending?.requestId === requestId ? this.pending : null;
        if (waiter) this.diagnostic({ type: 'request_timeout', operation: waiter.operation, requestId,
          elapsedMs: Date.now() - startedAt, expectedCount: waiter.expectedCount ?? undefined,
          receivedCount: waiter.frames.length, lastPhase: waiter.lastPhase, attempt, maxAttempts });
        if (this.pending?.requestId === requestId) this.pending = null;
        reject(waiter?.writesComplete && !waiter.decoderRejected
          ? new ReadTimeoutError('Perangkat tidak merespons') : new Error('Perangkat tidak merespons'));
      }, timeoutMs);
      this.pending = {
        requestId,
        frames: [],
        accept,
        operation,
        startedAt,
        lastPhase: 'request_started',
        expectedCount: null,
        writesComplete: false,
        decoderRejected: false,
        resolve: (response) => { globalThis.clearTimeout(timer); resolve(response); },
        reject: (error) => { globalThis.clearTimeout(timer); reject(error); },
      };
      void (async () => {
        try {
          for (const frame of frames) {
            if (this.pending?.requestId !== requestId) return;
            await control.writeValue(frame as unknown as BufferSource);
          }
          if (this.pending?.requestId === requestId) this.pending.writesComplete = true;
          if (this.pending?.requestId === requestId && this.pending.lastPhase === 'request_started') {
            this.pending.lastPhase = 'write_complete';
            this.diagnostic({ type: 'write_complete', operation, requestId, elapsedMs: Date.now() - startedAt });
          }
        } catch (error: unknown) {
          globalThis.clearTimeout(timer);
          if (this.pending?.requestId === requestId) this.pending = null;
          reject(error instanceof Error ? error : new Error('BLE write gagal'));
        }
      })();
    });
  }
}
