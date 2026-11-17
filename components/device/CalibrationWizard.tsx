import { useEffect, useRef, useState } from 'react';
import { Card, Input, Text, XStack, YStack } from 'tamagui';

import { createCalibrationApi, type CalibrationApi, type CalibrationCapabilities, type CalibrationCommand, type CalibrationCommandRequest, type CalibrationPreview, type CalibrationSensorId, type CalibrationState, type SensorDiagnostic } from '../../lib/api/calibration';
import { canBeginCalibration, calibrationStateLabel, commandSessionState, hasValidatedPreview, initialWizardStatus, nextStepAfterOperation, redactCalibrationError, type CalibrationWizardStep } from '../../lib/calibration/wizard';
import { AppButton } from '../AppButton';
import { DescriptionHint } from '../SectionHeader';
import { DeviceWorkflowSteps } from './DeviceWorkflowSteps';
import { CALIBRATION_STEPS, calibrationSensorLabel } from '../../lib/ui/deviceTools';
import { textStyles } from '../../lib/ui/typography';
import type { BleCalibrationResponse } from '../../lib/ble/protocol';
import type { DeviceBleSession } from '../../lib/ble/device-session';
import { Settings } from '../../lib/ui/icons';
import { decodeMq135GasResult, mq135EvidenceReady, mq135ProfileReferences, type Mq135GasResult } from '../../lib/calibration/mq135';
import { mq135ReferenceParameters } from '../../lib/calibration/mq135Reference';
import { Mq135GasDiagnostics, Mq135GasDiagnosticsUnavailable, Mq135ReferenceFields } from './Mq135Calibration';

export type CalibrationWizardProps = {
  deviceId: string;
  sensorId: CalibrationSensorId;
  diagnostic: SensorDiagnostic | null;
  api: CalibrationApi | null;
  apiFactory?: typeof createCalibrationApi;
  bleClient?: BleCalibrationAdapter | null;
  bleConnected?: boolean;
  peerBusy?: boolean;
  onConnectBle?: () => Promise<void>;
  transport?: 'https' | 'ble';
  onDiagnosticRefresh?: () => void;
  diagnosticAuthoritative?: boolean;
  onControlActivity?: DeviceBleSession['beginControlActivity'];
  compactTrigger?: boolean;
};

export type BleCalibrationAdapter = {
  verifyDeviceIdentity?: (deviceId: string) => Promise<string>;
  calibrationCapabilities?: (sensor: string) => Promise<BleCalibrationResponse>;
  calibrationEvidence?: (sensor: string) => Promise<BleCalibrationResponse>;
  calibrationStart: (sensor: string, parameters?: Record<string, string | number | boolean>) => Promise<BleCalibrationResponse>;
  calibrationCriteria: (sensor: string, parameters: Record<string, string | number | boolean>) => Promise<BleCalibrationResponse>;
  calibrationCapture: (sensor: string, anchor: 'tare' | 'reference' | 'empty' | 'full') => Promise<BleCalibrationResponse>;
  calibrationValidate: (sensor: string) => Promise<BleCalibrationResponse>;
  calibrationPreview: (sensor: string) => Promise<BleCalibrationResponse>;
  calibrationCommit: (sensor: string, expected?: Record<string, string | number | boolean>) => Promise<BleCalibrationResponse>;
  calibrationCancel: () => Promise<BleCalibrationResponse>;
  calibrationRollback?: (sensor: string, version: string, expectedHash?: string) => Promise<BleCalibrationResponse>;
};

function fieldValue(value: string): number | undefined {
  if (!value.trim()) return undefined;
  const parsed = Number(value);
  return Number.isFinite(parsed) ? parsed : undefined;
}

const POLL_DELAYS_MS = [100, 200, 400, 800, 1200, 2000, 3000, 5000] as const;
const TERMINAL_STATES: CalibrationState[] = ['COMMITTED', 'FAILED', 'EXPIRED', 'CANCELLED'];

function newSessionId(): string {
  if (typeof globalThis.crypto?.randomUUID === 'function') {
    return globalThis.crypto.randomUUID();
  }
  return `cal-${Date.now().toString(36)}-${Math.random().toString(36).slice(2)}`;
}

function parsePreview(value: unknown): CalibrationPreview | null {
  if (!value || typeof value !== 'object') return null;
  const candidate = value as Record<string, unknown>;
  const preview = (candidate.preview && typeof candidate.preview === 'object' ? candidate.preview : candidate) as Record<string, unknown>;
  if (typeof preview.proposal_hash !== 'string' || typeof preview.profile_version !== 'string') return null;
  if (typeof preview.evidence_count !== 'number' || typeof preview.evidence_hash !== 'string') return null;
  return {
    sensor_id: typeof preview.sensor_id === 'string' ? preview.sensor_id as CalibrationSensorId : '' as CalibrationSensorId,
    proposal_hash: preview.proposal_hash,
    profile_version: preview.profile_version,
    parameters: typeof preview.parameters === 'object' && preview.parameters !== null ? preview.parameters as CalibrationPreview['parameters'] : {},
    evidence_count: preview.evidence_count,
    evidence_hash: preview.evidence_hash,
    expires_at: typeof preview.expires_at === 'string' ? preview.expires_at :
      (typeof preview.expires_in_ms === 'number' ? new Date(Date.now() + preview.expires_in_ms).toISOString() : ''),
  };
}

function parseBleCapabilities(
  response: BleCalibrationResponse,
  sensorId: CalibrationSensorId,
): CalibrationCapabilities | null {
  if (response.status === 'ERROR' || response.sensor !== sensorId) return null;
  const fields = (name: 'references' | 'criteria' | 'engineering') => {
    const encoded = response.data[name];
    if (encoded === undefined) return [];
    if (encoded === 'none') return [];
    return encoded.split(',').map((entry) => {
      const separator = entry.indexOf(':');
      if (separator <= 0 || separator === entry.length - 1) return null;
      return { name: entry.slice(0, separator), unit: entry.slice(separator + 1), required: true };
    }).filter((field): field is { name: string; unit: string; required: boolean } => field !== null);
  };
  return {
    sensor_id: sensorId,
    references: fields('references'),
    criteria: fields('criteria'),
    engineering_parameters: fields('engineering'),
  };
}

function parseBleResult(response: BleCalibrationResponse): { state?: CalibrationState; preview?: CalibrationPreview; session_id?: string; error?: string } {
  try {
    if (response.status === 'ERROR') return { error: response.error ?? response.data.error ?? 'Perangkat menolak permintaan' };
    const data = response.data;
    const state = typeof data.state === 'string' ? data.state as CalibrationState : undefined;
    const preview = data.proposal_hash && data.profile_version
      ? {
          sensor_id: response.sensor as CalibrationSensorId,
          proposal_hash: data.proposal_hash,
          profile_version: data.profile_version,
          parameters: Object.fromEntries(Object.entries(data).filter(([key]) => !['proposal_hash', 'profile_version', 'evidence_count', 'evidence_hash', 'expires_at'].includes(key))),
          evidence_count: Number(data.evidence_count ?? 0),
          evidence_hash: data.evidence_hash ?? '',
          expires_at: data.expires_at ?? (data.expires_in_ms
            ? new Date(Date.now() + Number(data.expires_in_ms)).toISOString()
            : ''),
        } satisfies CalibrationPreview
      : undefined;
    return { state, session_id: response.sessionId, preview };
  } catch {
    return {};
  }
}

function parseBleDiagnostic(response: BleCalibrationResponse, sensorId: CalibrationSensorId): SensorDiagnostic | null {
  try {
    if (response.status === 'ERROR' || response.sensor !== sensorId) return null;
    const data = response.data;
    const flags = data.f ?? '';
    const optionalFlag = (name: string): boolean | undefined => {
      const match = new RegExp(`${name}=([01])`, 'u').exec(flags);
      return match ? match[1] === '1' : undefined;
    };
    const quality = data.q as SensorDiagnostic['quality'] | undefined;
    const reason = data.r;
    const fresh = data.fr === 'FRESH';
    if (!quality || !reason || !data.s) return null;
    const temperatureCrc = optionalFlag('tc');
    const humidityCrc = optionalFlag('hc');
    const directCrc = optionalFlag('crc');
    const crc = directCrc ?? (temperatureCrc !== undefined && humidityCrc !== undefined
      ? temperatureCrc && humidityCrc
      : undefined);
    return {
      sensor_id: sensorId,
      stream: data.st ?? 'unknown',
      quality,
      reason,
      raw: {
        present: data.raw === '1',
        identity_verified: optionalFlag('id'),
        crc_valid: crc,
        temperature_crc_valid: temperatureCrc,
        humidity_crc_valid: humidityCrc,
        conversion_complete: optionalFlag('conv'),
        heater_ready: optionalFlag('heat'),
      },
      converted_value: null,
      converted_unit: null,
      fresh,
      age_ms: data.age ? Number(data.age) / 1000 : null,
      ordering_unknown: false,
      future_clock_skew: false,
      last_sequence: null,
      profile_present: data.p === '1',
      profile_version: data.pv ?? null,
      profile_hash: null,
      calibration_state: (data.c as CalibrationState) ?? 'IDLE',
      accepted_samples: Number(data.a ?? 0),
      rejected_samples: Number(data.x ?? 0),
    };
  } catch { return null; }
}

export function previewAgreementKey(deviceId: string, sensorId: CalibrationSensorId, preview: CalibrationPreview): string {
  return JSON.stringify([deviceId, sensorId, preview.proposal_hash, preview.profile_version, preview.evidence_hash, preview.expires_at]);
}

export function canCommitApprovedPreview(
  preview: CalibrationPreview | null,
  deviceId: string,
  sensorId: CalibrationSensorId,
  approvedKey: string | null,
  nowMs = Date.now(),
): boolean {
  return hasValidatedPreview(preview, sensorId, nowMs) && approvedKey === previewAgreementKey(deviceId, sensorId, preview);
}

function storageKey(deviceId: string, sensorId: string): string { return `sigap.calibration.${deviceId}.${sensorId}`; }

export function CalibrationWizard({ deviceId, sensorId: physicalSensorId, diagnostic, api, apiFactory = createCalibrationApi, bleClient = null, bleConnected = false, peerBusy = false, onConnectBle, transport = 'https', onDiagnosticRefresh, diagnosticAuthoritative = false, onControlActivity, compactTrigger = false }: CalibrationWizardProps) {
  const [open, setOpen] = useState(false);
  const [step, setStep] = useState<CalibrationWizardStep>('diagnostics');
  const [sessionId, setSessionId] = useState<string | null>(null);
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const [reference, setReference] = useState('');
  const [criteria, setCriteria] = useState('');
  const [anchor, setAnchor] = useState('');
  const [preview, setPreview] = useState<CalibrationPreview | null>(null);
  const [approvedPreview, setApprovedPreview] = useState<string | null>(null);
  const [operatorToken, setOperatorToken] = useState('');
  const [csrfToken, setCsrfToken] = useState('');
  const [capabilities, setCapabilities] = useState<CalibrationCapabilities | null>(null);
  const [fields, setFields] = useState<Record<string, string>>({});
  const [commandId, setCommandId] = useState<string | null>(null);
  const [lastCommand, setLastCommand] = useState<CalibrationCommand | null>(null);
  const commandInFlight = useRef(false);
  const [sessionState, setSessionState] = useState(initialWizardStatus(diagnostic).state);
  const leaseRelease = useRef<(() => void) | null>(null);
  const [securedDiagnostic, setSecuredDiagnostic] = useState<SensorDiagnostic | null>(null);
  const [bleDiagnostic, setBleDiagnostic] = useState<SensorDiagnostic | null>(null);
  const [reading, setReading] = useState(false);
  const readAttempt = useRef(0);
  const [gasMode,setGasMode] = useState(false);
  const [gasEvidence,setGasEvidence] = useState<{value:Mq135GasResult;startedAtMs:number}|null>(null);
  const [gasCaptures,setGasCaptures] = useState(0);
  const acknowledgedCaptures = useRef(new Set<string>());
  const [rollbackApproval,setRollbackApproval] = useState<string|null>(null);
  const sensorId:CalibrationSensorId = physicalSensorId === 'mq135' && gasMode ? 'mq135_gas' : physicalSensorId;
  const isGas = sensorId === 'mq135_gas';
  const gasReferences = gasEvidence ? mq135ProfileReferences(gasEvidence.value) : null;
  const rollbackKey = gasReferences ? JSON.stringify([deviceId,sensorId,gasReferences.activeHash,gasReferences.previousHash]) : null;
  const activityActive = open || reading || busy || (sessionId !== null && !TERMINAL_STATES.includes(sessionState));
  useEffect(() => {
    if (!activityActive || !onControlActivity) return;
    const release = onControlActivity(transport === 'ble' ? 'ble-calibration' : 'https-calibration');
    leaseRelease.current = release;
    return () => {
      if (leaseRelease.current === release) leaseRelease.current = null;
      release();
    };
  }, [activityActive, onControlActivity, transport]);
  const renderContext = [deviceId, sensorId, transport, bleClient, bleConnected, api, apiFactory] as const;
  const latestRenderContext = useRef(renderContext);
  latestRenderContext.current = renderContext;
  const operationContext = useRef<AbortController | null>(null);
  useEffect(() => {
    const context = new AbortController();
    operationContext.current = context;
    return () => context.abort();
  }, [deviceId, sensorId, transport, bleClient, bleConnected, api, apiFactory]);
  const isCurrentContext = (context: AbortController | null): context is AbortController =>
    context !== null && operationContext.current === context && !context.signal.aborted &&
    renderContext.every((value, index) => Object.is(value, latestRenderContext.current[index]));
  const activeDiagnostic = transport === 'https' ? securedDiagnostic : (bleDiagnostic ?? (diagnosticAuthoritative ? diagnostic : null));
  const ready = capabilities !== null && (isGas
    ? gasEvidence !== null && mq135EvidenceReady(gasEvidence.value)
    : canBeginCalibration(activeDiagnostic));

  useEffect(() => {
    // A sensor/device/transport switch cannot reuse a proposal or session
    // identity from another calibration context.
    setPreview(null);
    setApprovedPreview(null);
    setStep('diagnostics');
    setSessionId(null);
    setCommandId(null);
    setSessionState('IDLE');
    setLastCommand(null);
    setError(null);
    setCapabilities(null);
    setSecuredDiagnostic(null);
    setBleDiagnostic(null);
    setFields({}); setGasEvidence(null); setGasCaptures(0); setRollbackApproval(null);
    acknowledgedCaptures.current.clear();
    // The activity effect keeps pending work leased until busy actually clears.
  }, [deviceId, sensorId, transport]);

  useEffect(() => () => { leaseRelease.current?.(); leaseRelease.current = null; }, []);

  useEffect(() => {
    if (typeof window === 'undefined') return;
    const value = window.sessionStorage.getItem(storageKey(deviceId, sensorId));
    if (!value) return;
    try {
      const saved = JSON.parse(value) as { commandId?: string; sessionId?: string };
      if (saved.commandId) setCommandId(saved.commandId);
      if (saved.sessionId) setSessionId(saved.sessionId);
    } catch { window.sessionStorage.removeItem(storageKey(deviceId, sensorId)); }
  }, [deviceId, sensorId]);

  const setPersistedCommand = (nextCommandId: string | null, nextSessionId: string | null) => {
    setCommandId(nextCommandId); setSessionId(nextSessionId);
    if (typeof window === 'undefined') return;
    const key = storageKey(deviceId, sensorId);
    if (!nextCommandId && !nextSessionId) window.sessionStorage.removeItem(key);
    else window.sessionStorage.setItem(key, JSON.stringify({ commandId: nextCommandId, sessionId: nextSessionId }));
  };

  useEffect(() => {
    if (transport !== 'ble' || bleConnected) return;
    // A disconnected BLE session cannot be resumed safely from browser state.
    // Clear proposal/session overlays so reconnect starts from device evidence.
    setBleDiagnostic(null); setGasEvidence(null); setRollbackApproval(null);
    setCapabilities(null);
    setPreview(null);
    setApprovedPreview(null);
    setSessionState('IDLE');
    setStep('diagnostics');
    setPersistedCommand(null, null);
  // The connection boundary intentionally clears all local calibration state.
  // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [transport, bleConnected, deviceId, sensorId]);

  const loadCapabilities = async (client: CalibrationApi, context: AbortController) => {
    if (!isCurrentContext(context)) return;
    try {
      const response = await client.getCapabilities(deviceId, sensorId, context.signal);
      if (isCurrentContext(context)) setCapabilities(response.capabilities ?? null);
    } catch (cause) { if (isCurrentContext(context)) setError(redactCalibrationError(cause)); }
  };

  const loadSecuredDiagnostic = async () => {
    const context = operationContext.current;
    if (!isCurrentContext(context) || commandInFlight.current) return;
    const client = api ?? (operatorToken.trim() ? apiFactory({ bearerToken: operatorToken.trim(), csrfToken: csrfToken.trim() }) : null);
    if (!client) { setError('Masukkan token sesi operator untuk memuat diagnostik terautentikasi.'); return; }
    setBusy(true); setError(null); commandInFlight.current = true;
    const release = onControlActivity?.('https-calibration');
    try {
      if (isGas) {
        setGasEvidence(null); setRollbackApproval(null);
        await loadCapabilities(client,context);
        if (!isCurrentContext(context)) return;
        const startedAtMs = performance.now();
        const queued = await client.submit({device_id:deviceId,sensor_id:sensorId,operation:'EVIDENCE',payload:{}},context.signal);
        if (!isCurrentContext(context)) return;
        const command = queued.command.status === 'QUEUED' || queued.command.status === 'DELIVERED'
          ? await pollCommand(client,queued.command.command_id,context) : queued.command;
        if (!isCurrentContext(context)) return;
        const value = command?.status === 'ACCEPTED' ? decodeMq135GasResult(command.result) : null;
        if (!value) throw new Error('Bukti MQ135 belum tersedia atau tidak valid.');
        setGasEvidence({value,startedAtMs}); return;
      }
      const response = await client.getDiagnostics(deviceId, sensorId, context.signal);
      if (!isCurrentContext(context)) return;
      if (!response.diagnostic) throw new Error('Diagnostik terautentikasi tidak tersedia');
      setSecuredDiagnostic(response.diagnostic);
      await loadCapabilities(client, context);
    } catch (cause) { if (isCurrentContext(context)) setError(redactCalibrationError(cause)); }
    finally { release?.(); commandInFlight.current = false; setBusy(false); }
  };

  const loadBleDiagnostic = async (context: AbortController) => {
    if (!isCurrentContext(context) || !bleClient?.calibrationEvidence) return;
    try {
      const startedAtMs = performance.now();
      if (isGas) setGasEvidence(null);
      const response = await bleClient.calibrationEvidence(sensorId);
      if (isGas) {
        const value = response.sensor === sensorId && response.status === 'ACCEPTED' ? decodeMq135GasResult(response.gas) : null;
        if (!value) throw new Error('Bukti MQ135 dari BLE tidak valid.');
        if (isCurrentContext(context)) setGasEvidence({value,startedAtMs}); return;
      }
      const value = parseBleDiagnostic(response, sensorId);
      if (isCurrentContext(context) && value) setBleDiagnostic(value);
    } catch (cause) { if (isCurrentContext(context)) setError(redactCalibrationError(cause)); }
  };

  const ensureBleIdentity = async () => {
    if (!bleClient?.verifyDeviceIdentity) {
      throw new Error('Adapter BLE tidak menyediakan verifikasi device_id');
    }
    await bleClient.verifyDeviceIdentity(deviceId);
  };

  useEffect(() => {
    if (!open || transport !== 'ble' || !bleConnected || peerBusy || !bleClient?.calibrationCapabilities) return;
    let current = true;
    const attempt = ++readAttempt.current;
    setReading(true);
    void (async () => {
      try {
        await ensureBleIdentity();
        if (!current) return;
        const capabilities = parseBleCapabilities(await bleClient.calibrationCapabilities!(sensorId), sensorId);
        if (!current) return;
        if (!capabilities) throw new Error('Kapabilitas perangkat tidak valid');
        setCapabilities(capabilities);
        if (bleClient.calibrationEvidence) {
          const startedAtMs = performance.now();
          const response = await bleClient.calibrationEvidence(sensorId);
          if (!current) return;
          if (isGas) {
            const value = response.sensor === sensorId && response.status === 'ACCEPTED' ? decodeMq135GasResult(response.gas) : null;
            if (!value) throw new Error('Bukti MQ135 dari BLE tidak valid.');
            setGasEvidence({value,startedAtMs}); return;
          }
          const diagnostic = parseBleDiagnostic(response, sensorId);
          if (current && diagnostic) setBleDiagnostic(diagnostic);
        }
      } catch (cause) { if (current) setError(redactCalibrationError(cause)); }
      finally { if (attempt === readAttempt.current) setReading(false); }
    })();
    // Closing/context replacement cannot admit later reads or late state commits.
    return () => { current = false; };
  // The adapter and connection flags intentionally define the physical read boundary.
  // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [open, transport, bleConnected, peerBusy, sensorId, deviceId, bleClient]);

  const applyCommand = (command: CalibrationCommand, context: AbortController) => {
    if (!isCurrentContext(context)) return;
    setLastCommand(command);
    const state = commandSessionState(command);
    setSessionState(state);
    const parsed = parsePreview(command.result);
    if (parsed && !parsed.expires_at && command.expires_at) parsed.expires_at = command.expires_at;
    if (parsed && hasValidatedPreview(parsed, sensorId)) {
      setPreview(parsed);
      setApprovedPreview(null);
    }
    const next = nextStepAfterOperation(command.operation, command.status === 'COMMITTED' ? 'COMMITTED' : state, step);
    if (isGas && command.operation.toUpperCase() === 'CAPTURE' && command.status === 'CAPTURING') {
      if (!acknowledgedCaptures.current.has(command.command_id)) {
        acknowledgedCaptures.current.add(command.command_id); setGasCaptures(count=>count+1);
      }
      setStep('capture');
    } else setStep(next);
    if (TERMINAL_STATES.includes(command.status)) {
      if (command.status === 'COMMITTED' || command.status === 'CANCELLED') onDiagnosticRefresh?.();
      if (command.status === 'CANCELLED') { setPreview(null); setApprovedPreview(null); }
      if (command.status !== 'FAILED' && command.status !== 'EXPIRED') setPersistedCommand(null, null);
    }
  };

  const pollCommand = async (client: CalibrationApi, id: string, context: AbortController): Promise<CalibrationCommand | null> => {
    let latest: CalibrationCommand | null = null;
    for (const delay of POLL_DELAYS_MS) {
      if (!isCurrentContext(context)) return null;
      latest = (await client.getCommand(deviceId, id, context.signal)).command;
      if (!isCurrentContext(context)) return null;
      applyCommand(latest, context);
      if (TERMINAL_STATES.includes(latest.status) ||
          (latest.status !== 'QUEUED' && latest.status !== 'DELIVERED')) return latest;
      await new Promise<void>((resolve) => {
        const finish = () => {
          globalThis.clearTimeout(timer);
          context.signal.removeEventListener('abort', finish);
          resolve();
        };
        const timer = globalThis.setTimeout(finish, delay);
        context.signal.addEventListener('abort', finish, { once: true });
        if (context.signal.aborted) finish();
      });
    }
    if (!latest) throw new Error('Status command tidak tersedia');
    return latest;
  };

  const submit = async (operation: CalibrationCommandRequest['operation'], extra: Partial<CalibrationCommandRequest> = {}) => {
    const context = operationContext.current;
    if (!isCurrentContext(context) || commandInFlight.current) return;
    if (operation === 'ROLLBACK' && (!isGas || !rollbackKey || rollbackApproval !== rollbackKey || !sessionId ||
        !gasReferences?.previousHash || extra.expected_profile_hash !== gasReferences.activeHash)) {
      setError('Persetujuan pemulihan untuk profil aktif diperlukan.'); return;
    }
    if (operation === 'COMMIT') {
      if (!canCommitApprovedPreview(preview, deviceId, sensorId, approvedPreview)) {
        setError('Persetujuan proposal terbaru diperlukan sebelum menyimpan.');
        return;
      }
    }
    const client = api ?? (transport === 'https' && operatorToken.trim() ? apiFactory({ bearerToken: operatorToken.trim(), csrfToken: csrfToken.trim() }) : null);
    if (!client && !bleClient) { setError('Sesi operator diperlukan untuk mode HTTPS, atau hubungkan BLE lokal.'); return; }
    setError(null); setBusy(true); commandInFlight.current = true;
    // An in-flight operation retains its own lease even if the view unmounts.
    const release = onControlActivity?.(transport === 'ble' ? 'ble-calibration' : 'https-calibration');
    try {
      if (transport === 'ble' && bleClient) {
        await ensureBleIdentity();
        if (!isCurrentContext(context)) return;
        const output = operation === 'START' ? await bleClient.calibrationStart(sensorId, {
            ...(extra.references ?? {}),
            ...(extra.parameters ?? {}),
          })
          : operation === 'SET_CRITERIA' ? await bleClient.calibrationCriteria(sensorId, extra.parameters ?? extra.criteria ?? {})
          : operation === 'CAPTURE' ? await bleClient.calibrationCapture(sensorId, isGas ? 'reference' : anchor as 'tare' | 'reference' | 'empty' | 'full')
          : operation === 'VALIDATE' ? await bleClient.calibrationValidate(sensorId)
          : operation === 'PREVIEW' ? await bleClient.calibrationPreview(sensorId)
          : operation === 'COMMIT' ? await bleClient.calibrationCommit(sensorId, {
              expected_profile_hash: extra.expected_profile_hash ?? preview?.proposal_hash ?? '',
              expected_profile_version: extra.expected_profile_version ?? preview?.profile_version ?? '',
            })
          : operation === 'ROLLBACK' && bleClient.calibrationRollback ? await bleClient.calibrationRollback(sensorId,extra.expected_profile_version??'',extra.expected_profile_hash)
          : operation === 'CANCEL' ? await bleClient.calibrationCancel()
          : (()=>{throw new Error('Operasi tidak didukung adapter BLE.');})();
        if (!isCurrentContext(context)) return;
        const parsed = parseBleResult(output);
        if (parsed.error) throw new Error(parsed.error);
        if (parsed.session_id) setPersistedCommand(null, parsed.session_id);
        if (parsed.preview && hasValidatedPreview(parsed.preview, sensorId)) {
          setPreview(parsed.preview);
          setApprovedPreview(null);
        }
        const stateByOperation: Record<CalibrationCommandRequest['operation'], CalibrationState> = { START: 'CAPTURING', SET_CRITERIA: 'CAPTURING', CAPTURE: 'VALIDATING', VALIDATE: 'PREVIEW_READY', PREVIEW: 'PREVIEW_READY', COMMIT: 'COMMITTED', CANCEL: 'CANCELLED', ROLLBACK: 'COMMITTED', EVIDENCE:'ACCEPTED', ACTIVE_PROFILE:'ACCEPTED', HISTORY:'ACCEPTED' };
        const state = parsed.state ?? stateByOperation[operation];
        setSessionState(state);
        if (isGas && operation === 'CAPTURE') {setGasCaptures(count=>count+1);setStep('capture');}
        else setStep(nextStepAfterOperation(operation, state, step));
        if (state === 'CANCELLED') { setPreview(null); setApprovedPreview(null); }
        if (state === 'COMMITTED' || state === 'CANCELLED') setPersistedCommand(null, null);
        await loadBleDiagnostic(context);
        if (isCurrentContext(context)) onDiagnosticRefresh?.();
        return;
      }
      if (!client) throw new Error('Operator session is unavailable');
      const effectiveSessionId = operation === 'START' ? (sessionId ?? newSessionId()) : sessionId;
      const response = await client.submit({ device_id: deviceId, sensor_id: sensorId, operation, session_id: effectiveSessionId ?? undefined, ...extra, ...(isGas && operation === 'CAPTURE' ? {payload:{anchor:'reference'}} : {}) }, context.signal);
      if (!isCurrentContext(context)) return;
      const returnedId = response.command?.command_id ?? null;
      if (returnedId || response.session_id || effectiveSessionId) {
        setPersistedCommand(returnedId ?? commandId, response.session_id ?? effectiveSessionId ?? null);
      }
      if (response.preview && hasValidatedPreview(response.preview, sensorId)) {
        setPreview(response.preview);
        setApprovedPreview(null);
      }
      const command = response.command;
      if (!command) throw new Error('Respons command tidak tersedia');
      applyCommand(command, context);
      if (returnedId && !TERMINAL_STATES.includes(command.status) && command.status !== 'PREVIEW_READY') await pollCommand(client, returnedId, context);
    } catch (cause) { if (isCurrentContext(context)) setError(redactCalibrationError(cause)); }
    finally { release?.(); commandInFlight.current = false; setBusy(false); }
  };

  const recoverCommand = async () => {
    const context = operationContext.current;
    if (!isCurrentContext(context)) return;
    if (!commandId || (!api && !operatorToken.trim()) || commandInFlight.current) return;
    const client = api ?? apiFactory({ bearerToken: operatorToken.trim(), csrfToken: csrfToken.trim() });
    commandInFlight.current = true; setBusy(true); setError(null);
    const release = onControlActivity?.('https-calibration');
    try { await pollCommand(client, commandId, context); }
    catch (cause) { if (isCurrentContext(context)) setError(redactCalibrationError(cause)); }
    finally { release?.(); commandInFlight.current = false; setBusy(false); }
  };

  const start = () => {
    if (!capabilities) { setError('Kapabilitas sensor belum dikonfirmasi oleh perangkat.'); return; }
    if (isGas) {setGasCaptures(0);void submit('START');return;}
    const refs = capabilities.references;
    const references: Record<string, number> = {};
    for (const field of refs.filter((value) => value.required)) {
      const parsed = fieldValue(fields[field.name] ?? (field.name === 'primary' ? reference : ''));
      if (parsed === undefined) { setError(`Referensi ${field.name} wajib diisi (${field.unit}).`); return; }
      references[field.name] = parsed;
    }
    const parameters: Record<string, number | string | boolean> = {};
    for (const field of capabilities.engineering_parameters.filter((value) => value.required)) {
      const raw = (fields[field.name] ?? '').trim();
      if (!raw) { setError(`Parameter ${field.name} wajib diisi (${field.unit}).`); return; }
      const parsed = Number(raw);
      parameters[field.name] = Number.isFinite(parsed) && raw !== '' ? parsed : raw;
    }
    void submit('START', { references, parameters });
  };
  const configure = () => {
    if (!capabilities) { setError('Kapabilitas sensor belum dikonfirmasi oleh perangkat.'); return; }
    if (isGas) {
      try {void submit('SET_CRITERIA',{parameters:mq135ReferenceParameters({...fields,response_mode:'normalized'})});}
      catch(cause) {setError(redactCalibrationError(cause));}
      return;
    }
    const criteriaValues: Record<string, number> = {};
    for (const field of capabilities.criteria.filter((value) => value.required)) {
      const parsed = fieldValue(fields[field.name] ?? (field.name === 'stability_window' ? criteria : ''));
      if (parsed === undefined) { setError(`Kriteria ${field.name} wajib diisi (${field.unit}).`); return; }
      criteriaValues[field.name] = parsed;
    }
    void submit('SET_CRITERIA', { criteria: criteriaValues });
  };
  const connectBle = async () => {
    const context = operationContext.current;
    if (!onConnectBle || !isCurrentContext(context) || commandInFlight.current) return;
    setError(null);
    setBusy(true); commandInFlight.current = true;
    const release = onControlActivity?.('ble-calibration');
    try {
      await onConnectBle();
    } catch (cause) {
      if (isCurrentContext(context)) setError(redactCalibrationError(cause));
    } finally {
      release?.(); commandInFlight.current = false; setBusy(false);
    }
  };

  return <>
    <YStack alignSelf={compactTrigger ? 'flex-end' : 'flex-start'} $deviceToolsMobile={compactTrigger ? { alignSelf: 'stretch' } : undefined}><AppButton variant={compactTrigger ? 'primary' : 'secondary'} size="sm" fullWidth={compactTrigger} onPress={() => { setOpen(true); setError(null); }} ariaLabel={`Kalibrasi ${sensorId}`}>{compactTrigger ? <><Settings size={16} color="#FFFFFF" aria-hidden /> Mulai</> : 'Kalibrasi sensor'}</AppButton></YStack>
    {open ? <Card role="dialog" aria-label="Wizard kalibrasi sensor" borderWidth={1} borderColor="$borderColor" backgroundColor="$surface" borderRadius="$4" padding="$4" gap="$3">
      <XStack justifyContent="space-between" alignItems="center" gap="$2" flexWrap="wrap"><YStack flex={1} minWidth={180} gap="$1"><Text color="$color" {...textStyles.sectionTitle}>Kalibrasi sensor</Text><Text color="$colorMuted" {...textStyles.bodySecondary}>{calibrationSensorLabel(sensorId)} · {transport === 'ble' ? 'BLE lokal' : 'HTTPS Wi-Fi'}</Text></YStack><DescriptionHint description={`Perangkat ${deviceId}. Kalibrasi memakai bukti perangkat, bukan data sintetis. Referensi fisik dan proposal tervalidasi tetap diperlukan; otorisasi HTTPS terpisah dari BLE.`} ariaLabel="Info bukti kalibrasi" /><AppButton variant="ghost" size="sm" onPress={() => setOpen(false)} ariaLabel="Tutup wizard kalibrasi">Tutup</AppButton></XStack>
      {physicalSensorId === 'mq135' ? <XStack gap="$2" flexWrap="wrap"><AppButton variant="secondary" size="sm" ariaLabel="Kalibrasi ADC MQ135" aria-pressed={!gasMode} disabled={busy || reading || sessionId !== null} onPress={()=>setGasMode(false)}>ADC</AppButton><AppButton variant="secondary" size="sm" ariaLabel="Respons Gas Relatif" aria-pressed={gasMode} disabled={busy || reading || sessionId !== null} onPress={()=>setGasMode(true)}>Respons Gas Relatif</AppButton></XStack> : null}
      <DeviceWorkflowSteps steps={CALIBRATION_STEPS} current={CALIBRATION_STEPS.findIndex(item => item.id === step)} />
      <Text color="$colorMuted" {...textStyles.caption} role="status">{calibrationStateLabel(sessionState)}</Text>
      {transport === 'ble' && peerBusy ? <Text role="status" color="$colorMuted">Menunggu operasi BLE selesai...</Text> : null}
      {transport === 'ble' && !bleConnected ? <YStack gap="$2"><Text color="$statusWaspada">BLE belum terhubung.</Text><AppButton onPress={() => void connectBle()} disabled={busy || peerBusy || !onConnectBle} ariaLabel="Hubungkan BLE kalibrasi">Hubungkan BLE</AppButton></YStack> : null}
      {transport === 'https' && !api ? <YStack gap="$2"><Text color="$color" fontWeight="700">Sesi operator</Text><Input minHeight={44} fontSize={16} value={operatorToken} onChangeText={(value) => { setOperatorToken(value); setSecuredDiagnostic(null); }} placeholder="Token sesi operator (tidak disimpan)" type="password" secureTextEntry aria-label="Token sesi operator" maxLength={512} autoCapitalize="none" /><Input minHeight={44} fontSize={16} value={csrfToken} onChangeText={setCsrfToken} placeholder="Token CSRF sesi (tidak disimpan)" type="password" secureTextEntry aria-label="Token CSRF sesi operator" maxLength={512} autoCapitalize="none" /><AppButton variant="secondary" size="sm" onPress={() => void loadSecuredDiagnostic()} disabled={busy || !operatorToken.trim()} ariaLabel="Muat diagnostik terautentikasi">Muat diagnostik terautentikasi</AppButton></YStack> : null}
      {transport === 'https' && commandId ? <YStack gap="$2"><Text color="$colorMuted" fontSize="$2">Perintah {commandId} · status {lastCommand?.status ?? sessionState}</Text><AppButton variant="ghost" size="sm" onPress={() => void recoverCommand()} disabled={busy || (!api && !operatorToken.trim())} ariaLabel="Pulihkan status perintah">Pulihkan status perintah</AppButton></YStack> : null}
      {isGas ? gasEvidence ? <Mq135GasDiagnostics value={gasEvidence.value} startedAtMs={gasEvidence.startedAtMs}/> : <Mq135GasDiagnosticsUnavailable/> : null}
      {transport === 'https' && api ? <AppButton variant="secondary" size="sm" onPress={()=>void loadSecuredDiagnostic()} disabled={busy || reading} ariaLabel="Muat bukti perangkat">Muat Bukti</AppButton> : null}
      {(!isGas && !activeDiagnostic) || (isGas && !gasEvidence) ? <Text color="$colorMuted">Diagnostik belum tersedia.</Text> : <>
        <Text color={ready ? '$statusNormal' : '$statusWaspada'}>{isGas ? (ready ? 'Profil ADC dan raw tersedia. Freshness serta stabilitas setiap sampel tetap diperiksa perangkat setelah kriteria dikonfirmasi.' : 'Profil ADC terukur dan bukti perangkat yang valid diperlukan.') : ready ? 'Bukti raw segar tersedia untuk validasi.' : 'Bukti atau kapabilitas perangkat belum layak untuk capture. Periksa quality, CRC, identitas, freshness, dan kapabilitas.'}</Text>
        {isGas ? <Mq135ReferenceFields values={fields} onChange={(name,value)=>setFields(previous=>({...previous,[name]:value}))} disabled={busy || step!=='criteria'}/> : capabilities ? <><YStack gap="$2"><Text color="$color" fontWeight="700">Referensi fisik</Text>{capabilities.references.map((field) => <Input key={field.name} minHeight={44} fontSize={16} value={fields[field.name] ?? (field.name === 'primary' ? reference : '')} onChangeText={(value) => { setFields((previous) => ({ ...previous, [field.name]: value })); if (field.name === 'primary') setReference(value); }} placeholder={`${field.name} (${field.unit})${field.required ? ' wajib' : ''}`} keyboardType="numeric" aria-label={`Referensi ${field.name}`} maxLength={32} />)}</YStack>
        {(step === 'criteria' || step === 'capture' || step === 'validate' || step === 'preview' || step === 'commit') ? <YStack gap="$2"><Text color="$color" fontWeight="700">Kriteria eksplisit</Text>{capabilities.criteria.map((field) => <Input key={field.name} minHeight={44} fontSize={16} value={fields[field.name] ?? (field.name === 'stability_window' ? criteria : '')} onChangeText={(value) => { setFields((previous) => ({ ...previous, [field.name]: value })); if (field.name === 'stability_window') setCriteria(value); }} placeholder={`${field.name} (${field.unit})${field.required ? ' wajib' : ''}`} keyboardType="numeric" aria-label={`Kriteria ${field.name}`} maxLength={32} />)}</YStack> : null}
        {capabilities.engineering_parameters.length ? <YStack gap="$2"><Text color="$color" fontWeight="700">Parameter engineering</Text>{capabilities.engineering_parameters.map((field) => <Input key={field.name} minHeight={44} fontSize={16} value={fields[field.name] ?? ''} onChangeText={(value) => setFields((previous) => ({ ...previous, [field.name]: value }))} placeholder={`${field.name} (${field.unit})${field.required ? ' wajib' : ''}`} maxLength={64} aria-label={`Parameter ${field.name}`} />)}</YStack> : null}</> : <Text color="$colorMuted">Menunggu kapabilitas sensor dari perangkat.</Text>}
        <XStack gap="$2" flexWrap="wrap">
          {step === 'diagnostics' ? <AppButton onPress={start} disabled={busy || !ready} ariaLabel="Mulai kalibrasi">Mulai</AppButton> : null}
          {step === 'criteria' ? <AppButton onPress={configure} disabled={busy} ariaLabel="Simpan kriteria">Simpan kriteria</AppButton> : null}
          {step === 'capture' && !isGas ? <YStack gap="$2" minWidth={180}><Text color="$color" fontWeight="700">Anchor</Text><Input minHeight={44} fontSize={16} value={anchor} onChangeText={setAnchor} placeholder="tare/reference/empty/full" aria-label="Anchor kalibrasi" maxLength={16} /></YStack> : null}
          {isGas && step === 'capture' ? <YStack gap="$2"><Text color="$colorMuted" role="status">Permintaan sampel diterima: {gasCaptures}. Statistik final berasal dari validasi perangkat.</Text><AppButton variant="secondary" onPress={()=>setStep('validate')} disabled={busy || !Number.isInteger(Number(fields.samples)) || Number(fields.samples)<3 || gasCaptures<Number(fields.samples)} ariaLabel="Lanjut ke validasi referensi">Lanjut Validasi</AppButton></YStack> : null}
          {step === 'capture' || step === 'validate' ? <AppButton onPress={() => void submit(step === 'capture' ? 'CAPTURE' : 'VALIDATE')} disabled={busy || (step === 'capture' && !isGas && !['tare', 'reference', 'empty', 'full'].includes(anchor))} ariaLabel={step === 'capture' ? 'Ambil sampel' : 'Validasi sampel'}>{step === 'capture' ? (isGas ? 'Ambil Referensi' : 'Ambil sampel') : 'Validasi'}</AppButton> : null}
          {step === 'preview' ? <AppButton onPress={() => void submit('PREVIEW')} disabled={busy} ariaLabel="Buat preview">Buat preview</AppButton> : null}
          {step === 'commit' ? <AppButton onPress={() => void submit('COMMIT', { expected_profile_hash: preview?.proposal_hash, expected_profile_version: preview?.profile_version })} disabled={busy || !canCommitApprovedPreview(preview, deviceId, sensorId, approvedPreview)} ariaLabel="Commit profil">Commit profil</AppButton> : null}
          {step !== 'done' ? <AppButton variant="danger" onPress={() => void submit('CANCEL')} disabled={busy || !sessionId} ariaLabel="Batalkan kalibrasi">Batalkan</AppButton> : null}
          <AppButton variant="ghost" onPress={isGas ? ()=>void (transport==='https'?loadSecuredDiagnostic():loadBleDiagnostic(operationContext.current!)) : onDiagnosticRefresh} disabled={busy || reading || (transport==='ble'&&!bleConnected)} ariaLabel="Perbarui bukti sensor">Perbarui bukti</AppButton>
        </XStack>
        {isGas && gasReferences?.previousHash && gasReferences.activeHash && sessionId ? <YStack gap="$2"><Text color="$colorMuted">Versi sebelumnya: {gasReferences.previousVersion}. Pemulihan tidak mengubah profil ADC.</Text><AppButton variant="secondary" size="sm" disabled={busy} ariaLabel="Setujui pemulihan profil gas" aria-pressed={rollbackApproval===rollbackKey} onPress={()=>setRollbackApproval(value=>value===rollbackKey?null:rollbackKey)}>Setujui Pemulihan</AppButton><AppButton variant="secondary" size="sm" disabled={busy || rollbackApproval!==rollbackKey || !rollbackKey} ariaLabel="Pulihkan profil gas sebelumnya" onPress={()=>void submit('ROLLBACK',{expected_profile_version:gasReferences.activeVersion??'',expected_profile_hash:gasReferences.activeHash??''})}>Pulihkan</AppButton></YStack> : null}
        {preview ? <Card borderWidth={1} borderColor="$primarySoft" backgroundColor="$primarySoft" padding="$3" borderRadius="$3" gap="$1"><Text color="$color" fontWeight="700">Proposal perangkat</Text><Text color="$colorMuted" fontSize="$2">Sensor {preview.sensor_id} · perangkat {deviceId}</Text><Text color="$colorMuted" fontSize="$2">Versi profil {preview.profile_version} · bukti {preview.evidence_count} · berlaku sampai {preview.expires_at}</Text><Text color="$color" fontWeight="700" fontSize="$2">Parameter usulan</Text>{Object.entries(preview.parameters).map(([name, value]) => <Text key={name} color="$color" fontSize="$2">{name}: {String(value)}</Text>)}{step === 'commit' ? <XStack alignItems="center" gap="$2" marginTop="$2"><AppButton variant="secondary" size="sm" ariaLabel="Setujui proposal kalibrasi" aria-pressed={approvedPreview === previewAgreementKey(deviceId, sensorId, preview)} onPress={() => setApprovedPreview((value) => value === previewAgreementKey(deviceId, sensorId, preview) ? null : previewAgreementKey(deviceId, sensorId, preview))}>{approvedPreview === previewAgreementKey(deviceId, sensorId, preview) ? '✓ Disetujui' : 'Setujui proposal'}</AppButton><Text color="$color" fontSize="$2">Persetujuan hanya berlaku untuk proposal ini.</Text></XStack> : null}</Card> : null}
      </>}
      {error ? <Text role="alert" color="$statusSafetyAlert">{error}</Text> : null}
      {busy || reading ? <Text color="$colorMuted" role="status">Memproses permintaan...</Text> : null}
    </Card> : null}
  </>;
}
