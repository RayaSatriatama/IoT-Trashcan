import { useEffect, useRef, useState } from 'react';
import { Card, Input, Text, XStack, YStack } from 'tamagui';

import { createCalibrationApi, type CalibrationApi, type CalibrationCapabilities, type CalibrationCommand, type CalibrationCommandRequest, type CalibrationPreview, type CalibrationSensorId, type CalibrationState, type SensorDiagnostic } from '../../lib/api/calibration';
import { canBeginCalibration, calibrationStateLabel, initialWizardStatus, nextStep, redactCalibrationError, type CalibrationWizardStep } from '../../lib/calibration/wizard';
import { AppButton } from '../AppButton';

export type CalibrationWizardProps = {
  deviceId: string;
  sensorId: CalibrationSensorId;
  diagnostic: SensorDiagnostic | null;
  api: CalibrationApi | null;
  apiFactory?: typeof createCalibrationApi;
  bleClient?: BleCalibrationAdapter | null;
  bleConnected?: boolean;
  onConnectBle?: () => Promise<void>;
  transport?: 'https' | 'ble';
  onDiagnosticRefresh?: () => void;
  diagnosticAuthoritative?: boolean;
};

export type BleCalibrationAdapter = {
  calibrationEvidence?: (sensor: string) => Promise<string>;
  calibrationStart: (sensor: string) => Promise<string>;
  calibrationCriteria: (sensor: string, parameters: Record<string, string | number | boolean>) => Promise<string>;
  calibrationCapture: (sensor: string, anchor: 'tare' | 'reference' | 'empty' | 'full') => Promise<string>;
  calibrationValidate: (sensor: string) => Promise<string>;
  calibrationPreview: (sensor: string) => Promise<string>;
  calibrationCommit: (sensor: string) => Promise<string>;
  calibrationCancel: () => Promise<string>;
};

function fieldValue(value: string): number | undefined {
  if (!value.trim()) return undefined;
  const parsed = Number(value);
  return Number.isFinite(parsed) ? parsed : undefined;
}

const POLL_DELAYS_MS = [100, 200, 400, 800, 1200, 2000, 3000, 5000] as const;
const TERMINAL_STATES: CalibrationState[] = ['COMMITTED', 'FAILED', 'EXPIRED', 'CANCELLED'];

function parsePreview(value: unknown): CalibrationPreview | null {
  if (!value || typeof value !== 'object') return null;
  const candidate = value as Record<string, unknown>;
  const preview = (candidate.preview && typeof candidate.preview === 'object' ? candidate.preview : candidate) as Record<string, unknown>;
  if (typeof preview.proposal_hash !== 'string' || typeof preview.profile_version !== 'string') return null;
  if (typeof preview.evidence_count !== 'number' || typeof preview.evidence_hash !== 'string' || typeof preview.expires_at !== 'string') return null;
  return {
    sensor_id: typeof preview.sensor_id === 'string' ? preview.sensor_id as CalibrationSensorId : '' as CalibrationSensorId,
    proposal_hash: preview.proposal_hash,
    profile_version: preview.profile_version,
    parameters: typeof preview.parameters === 'object' && preview.parameters !== null ? preview.parameters as CalibrationPreview['parameters'] : {},
    evidence_count: preview.evidence_count,
    evidence_hash: preview.evidence_hash,
    expires_at: preview.expires_at,
  };
}

function parseBleResult(text: string): { state?: CalibrationState; preview?: CalibrationPreview; session_id?: string; error?: string } {
  try {
    const value = JSON.parse(text) as Record<string, unknown>;
    return {
      state: typeof value.state === 'string' ? value.state as CalibrationState : undefined,
      session_id: typeof value.session_id === 'string' ? value.session_id : undefined,
      preview: parsePreview(value) ?? undefined,
      error: typeof value.error === 'string' ? value.error : undefined,
    };
  } catch {
    return {};
  }
}

function parseBleDiagnostic(text: string, sensorId: CalibrationSensorId): SensorDiagnostic | null {
  try {
    const value = JSON.parse(text) as Partial<SensorDiagnostic>;
    if (value.sensor_id !== sensorId || typeof value.quality !== 'string' || typeof value.reason !== 'string' || typeof value.fresh !== 'boolean') return null;
    if (!value.raw || typeof value.raw !== 'object') return null;
    return value as SensorDiagnostic;
  } catch { return null; }
}

function storageKey(deviceId: string, sensorId: string): string { return `sigap.calibration.${deviceId}.${sensorId}`; }

export function CalibrationWizard({ deviceId, sensorId, diagnostic, api, apiFactory = createCalibrationApi, bleClient = null, bleConnected = false, onConnectBle, transport = 'https', onDiagnosticRefresh, diagnosticAuthoritative = false }: CalibrationWizardProps) {
  const [open, setOpen] = useState(false);
  const [step, setStep] = useState<CalibrationWizardStep>('diagnostics');
  const [sessionId, setSessionId] = useState<string | null>(null);
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const [reference, setReference] = useState('');
  const [criteria, setCriteria] = useState('');
  const [anchor, setAnchor] = useState('');
  const [preview, setPreview] = useState<CalibrationPreview | null>(null);
  const [operatorToken, setOperatorToken] = useState('');
  const [capabilities, setCapabilities] = useState<CalibrationCapabilities | null>(null);
  const [fields, setFields] = useState<Record<string, string>>({});
  const [commandId, setCommandId] = useState<string | null>(null);
  const [lastCommand, setLastCommand] = useState<CalibrationCommand | null>(null);
  const commandInFlight = useRef(false);
  const [sessionState, setSessionState] = useState(initialWizardStatus(diagnostic).state);
  const [securedDiagnostic, setSecuredDiagnostic] = useState<SensorDiagnostic | null>(null);
  const [bleDiagnostic, setBleDiagnostic] = useState<SensorDiagnostic | null>(null);
  const activeDiagnostic = transport === 'https' ? securedDiagnostic : (bleDiagnostic ?? (diagnosticAuthoritative ? diagnostic : null));
  const ready = canBeginCalibration(activeDiagnostic);

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

  const loadCapabilities = async (client: CalibrationApi) => {
    try {
      const response = await client.getCapabilities(deviceId, sensorId);
      setCapabilities(response.capabilities ?? null);
    } catch (cause) { setError(redactCalibrationError(cause)); }
  };

  const loadSecuredDiagnostic = async () => {
    const client = api ?? (operatorToken.trim() ? apiFactory({ bearerToken: operatorToken.trim() }) : null);
    if (!client) { setError('Masukkan token sesi operator untuk memuat diagnostik terautentikasi.'); return; }
    setBusy(true); setError(null);
    try {
      const response = await client.getDiagnostics(deviceId, sensorId);
      if (!response.diagnostic) throw new Error('Diagnostik terautentikasi tidak tersedia');
      setSecuredDiagnostic(response.diagnostic);
      await loadCapabilities(client);
    } catch (cause) { setError(redactCalibrationError(cause)); }
    finally { setBusy(false); }
  };

  const loadBleDiagnostic = async () => {
    if (!bleClient?.calibrationEvidence) return;
    try {
      const value = parseBleDiagnostic(await bleClient.calibrationEvidence(sensorId), sensorId);
      if (value) setBleDiagnostic(value);
    } catch (cause) { setError(redactCalibrationError(cause)); }
  };

  useEffect(() => {
    if (open && transport === 'ble' && bleConnected) void loadBleDiagnostic();
  // The adapter and connection flags intentionally define the physical read boundary.
  // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [open, transport, bleConnected, sensorId]);

  const applyCommand = (command: CalibrationCommand) => {
    setLastCommand(command);
    setSessionState(command.status);
    const parsed = parsePreview(command.result);
    if (parsed) setPreview(parsed);
    const next = nextStep(command.status, step);
    setStep(next);
    if (TERMINAL_STATES.includes(command.status)) {
      if (command.status === 'COMMITTED' || command.status === 'CANCELLED') onDiagnosticRefresh?.();
      if (command.status !== 'FAILED' && command.status !== 'EXPIRED') setPersistedCommand(null, null);
    }
  };

  const pollCommand = async (client: CalibrationApi, id: string): Promise<CalibrationCommand> => {
    let latest: CalibrationCommand | null = null;
    for (const delay of POLL_DELAYS_MS) {
      latest = (await client.getCommand(deviceId, id)).command;
      applyCommand(latest);
      if (TERMINAL_STATES.includes(latest.status) || latest.status === 'PREVIEW_READY') return latest;
      await new Promise<void>((resolve) => globalThis.setTimeout(resolve, delay));
    }
    if (!latest) throw new Error('Status command tidak tersedia');
    return latest;
  };

  const submit = async (operation: CalibrationCommandRequest['operation'], extra: Partial<CalibrationCommandRequest> = {}) => {
    const client = api ?? (transport === 'https' && operatorToken.trim() ? apiFactory({ bearerToken: operatorToken.trim() }) : null);
    if (!client && !bleClient) { setError('Sesi operator diperlukan untuk mode HTTPS, atau hubungkan BLE lokal.'); return; }
    if (commandInFlight.current && transport === 'https') return;
    setError(null); setBusy(true); commandInFlight.current = true;
    try {
      if (transport === 'ble' && bleClient) {
        const output = operation === 'START' ? await bleClient.calibrationStart(sensorId)
          : operation === 'SET_CRITERIA' ? await bleClient.calibrationCriteria(sensorId, extra.criteria ?? {})
          : operation === 'CAPTURE' ? await bleClient.calibrationCapture(sensorId, anchor as 'tare' | 'reference' | 'empty' | 'full')
          : operation === 'VALIDATE' ? await bleClient.calibrationValidate(sensorId)
          : operation === 'PREVIEW' ? await bleClient.calibrationPreview(sensorId)
          : operation === 'COMMIT' ? await bleClient.calibrationCommit(sensorId)
          : await bleClient.calibrationCancel();
        const parsed = parseBleResult(output);
        if (parsed.error) throw new Error(parsed.error);
        if (parsed.session_id) setPersistedCommand(null, parsed.session_id);
        if (parsed.preview) setPreview(parsed.preview);
        const stateByOperation: Record<CalibrationCommandRequest['operation'], CalibrationState> = { START: 'CAPTURING', SET_CRITERIA: 'CAPTURING', CAPTURE: 'VALIDATING', VALIDATE: 'PREVIEW_READY', PREVIEW: 'PREVIEW_READY', COMMIT: 'COMMITTED', CANCEL: 'CANCELLED', ROLLBACK: 'FAILED' };
        const state = parsed.state ?? stateByOperation[operation];
        setSessionState(state);
        setStep(nextStep(state, step));
        if (state === 'COMMITTED' || state === 'CANCELLED') setPersistedCommand(null, null);
        await loadBleDiagnostic();
        onDiagnosticRefresh?.();
        return;
      }
      if (!client) throw new Error('Operator session is unavailable');
      const response = await client.submit({ device_id: deviceId, sensor_id: sensorId, operation, session_id: sessionId ?? undefined, ...extra });
      const returnedId = response.command?.command_id ?? null;
      if (returnedId || response.session_id) setPersistedCommand(returnedId ?? commandId, response.session_id ?? sessionId);
      if (response.preview) setPreview(response.preview);
      const command = response.command;
      if (!command) throw new Error('Respons command tidak tersedia');
      applyCommand(command);
      if (returnedId && !TERMINAL_STATES.includes(command.status) && command.status !== 'PREVIEW_READY') await pollCommand(client, returnedId);
    } catch (cause) { setError(redactCalibrationError(cause)); }
    finally { commandInFlight.current = false; setBusy(false); }
  };

  const recoverCommand = async () => {
    if (!commandId || !operatorToken.trim() || commandInFlight.current) return;
    const client = api ?? apiFactory({ bearerToken: operatorToken.trim() });
    commandInFlight.current = true; setBusy(true); setError(null);
    try { await pollCommand(client, commandId); }
    catch (cause) { setError(redactCalibrationError(cause)); }
    finally { commandInFlight.current = false; setBusy(false); }
  };

  const start = () => {
    const refs = capabilities?.references ?? [{ name: 'primary', unit: 'typed', required: true }];
    const references: Record<string, number> = {};
    for (const field of refs.filter((value) => value.required)) {
      const parsed = fieldValue(fields[field.name] ?? (field.name === 'primary' ? reference : ''));
      if (parsed === undefined) { setError(`Referensi ${field.name} wajib diisi (${field.unit}).`); return; }
      references[field.name] = parsed;
    }
    const parameters: Record<string, number | string | boolean> = {};
    for (const field of (capabilities?.engineering_parameters ?? []).filter((value) => value.required)) {
      const raw = (fields[field.name] ?? '').trim();
      if (!raw) { setError(`Parameter ${field.name} wajib diisi (${field.unit}).`); return; }
      const parsed = Number(raw);
      parameters[field.name] = Number.isFinite(parsed) && raw !== '' ? parsed : raw;
    }
    void submit('START', { references, parameters });
  };
  const configure = () => {
    const criteriaValues: Record<string, number> = {};
    for (const field of (capabilities?.criteria ?? [{ name: 'stability_window', unit: 'samples', required: true }]).filter((value) => value.required)) {
      const parsed = fieldValue(fields[field.name] ?? (field.name === 'stability_window' ? criteria : ''));
      if (parsed === undefined) { setError(`Kriteria ${field.name} wajib diisi (${field.unit}).`); return; }
      criteriaValues[field.name] = parsed;
    }
    void submit('SET_CRITERIA', { criteria: criteriaValues });
  };

  return <>
    <AppButton variant="secondary" size="sm" onPress={() => { setOpen(true); setError(null); }} ariaLabel={`Kalibrasi ${sensorId}`}>Kalibrasi sensor</AppButton>
    {open ? <Card role="dialog" aria-label="Wizard kalibrasi sensor" borderWidth={1} borderColor="$borderColor" backgroundColor="$surface" borderRadius="$4" padding="$4" gap="$3">
      <XStack justifyContent="space-between" alignItems="center" gap="$3"><YStack flex={1} gap="$1"><Text color="$color" fontSize="$5" fontWeight="800">Kalibrasi {sensorId}</Text><Text color="$colorMuted" fontSize="$2">Perangkat {deviceId} · transport {transport === 'ble' ? 'BLE lokal' : 'HTTPS Wi-Fi'}</Text></YStack><AppButton variant="ghost" size="sm" onPress={() => setOpen(false)} ariaLabel="Tutup wizard kalibrasi">Tutup</AppButton></XStack>
      <Text color="$colorMuted" fontSize="$2">Langkah: {step} · {calibrationStateLabel(sessionState)}</Text>
      {transport === 'ble' && !bleConnected ? <YStack gap="$2"><Text color="$statusWaspada">BLE belum terhubung. Browser harus mendukung Web Bluetooth pada HTTPS.</Text><AppButton onPress={() => void onConnectBle?.()} disabled={busy || !onConnectBle} ariaLabel="Hubungkan BLE kalibrasi">Hubungkan BLE</AppButton></YStack> : null}
      {transport === 'https' && !api ? <YStack gap="$2"><Text color="$color" fontWeight="700">Sesi operator</Text><Input value={operatorToken} onChangeText={(value) => { setOperatorToken(value); setSecuredDiagnostic(null); }} placeholder="Token sesi operator (tidak disimpan)" secureTextEntry aria-label="Token sesi operator" maxLength={512} autoCapitalize="none" /><AppButton variant="secondary" size="sm" onPress={() => void loadSecuredDiagnostic()} disabled={busy || !operatorToken.trim()} ariaLabel="Muat diagnostik terautentikasi">Muat diagnostik terautentikasi</AppButton></YStack> : null}
      {transport === 'https' && commandId ? <YStack gap="$2"><Text color="$colorMuted" fontSize="$2">Perintah {commandId} · status {lastCommand?.status ?? sessionState}</Text><AppButton variant="ghost" size="sm" onPress={() => void recoverCommand()} disabled={busy || !operatorToken.trim()} ariaLabel="Pulihkan status perintah">Pulihkan status perintah</AppButton></YStack> : null}
      {!activeDiagnostic ? <Text color="$colorMuted">Diagnostik belum tersedia. Tidak ada bukti sintetis yang digunakan.</Text> : <>
        <Text color={ready ? '$statusNormal' : '$statusWaspada'}>{ready ? 'Bukti raw segar tersedia untuk validasi.' : 'Bukti belum layak untuk capture. Periksa quality, CRC, identitas, dan freshness.'}</Text>
        <YStack gap="$2"><Text color="$color" fontWeight="700">Referensi fisik</Text>{(capabilities?.references ?? [{ name: 'primary', unit: 'typed', required: true }]).map((field) => <Input key={field.name} value={fields[field.name] ?? (field.name === 'primary' ? reference : '')} onChangeText={(value) => { setFields((previous) => ({ ...previous, [field.name]: value })); if (field.name === 'primary') setReference(value); }} placeholder={`${field.name} (${field.unit})${field.required ? ' wajib' : ''}`} keyboardType="numeric" aria-label={`Referensi ${field.name}`} maxLength={32} />)}</YStack>
        {(step === 'criteria' || step === 'capture' || step === 'validate' || step === 'preview' || step === 'commit') ? <YStack gap="$2"><Text color="$color" fontWeight="700">Kriteria eksplisit</Text>{(capabilities?.criteria ?? [{ name: 'stability_window', unit: 'samples', required: true }]).map((field) => <Input key={field.name} value={fields[field.name] ?? (field.name === 'stability_window' ? criteria : '')} onChangeText={(value) => { setFields((previous) => ({ ...previous, [field.name]: value })); if (field.name === 'stability_window') setCriteria(value); }} placeholder={`${field.name} (${field.unit})${field.required ? ' wajib' : ''}`} keyboardType="numeric" aria-label={`Kriteria ${field.name}`} maxLength={32} />)}</YStack> : null}
        {capabilities?.engineering_parameters.length ? <YStack gap="$2"><Text color="$color" fontWeight="700">Parameter engineering</Text>{capabilities.engineering_parameters.map((field) => <Input key={field.name} value={fields[field.name] ?? ''} onChangeText={(value) => setFields((previous) => ({ ...previous, [field.name]: value }))} placeholder={`${field.name} (${field.unit})${field.required ? ' wajib' : ''}`} maxLength={64} aria-label={`Parameter ${field.name}`} />)}</YStack> : null}
        <XStack gap="$2" flexWrap="wrap">
          {step === 'diagnostics' ? <AppButton onPress={start} disabled={busy || !ready} ariaLabel="Mulai kalibrasi">Mulai</AppButton> : null}
          {step === 'criteria' ? <AppButton onPress={configure} disabled={busy} ariaLabel="Simpan kriteria">Simpan kriteria</AppButton> : null}
          {step === 'capture' ? <YStack gap="$2" minWidth={180}><Text color="$color" fontWeight="700">Anchor</Text><Input value={anchor} onChangeText={setAnchor} placeholder="tare/reference/empty/full" aria-label="Anchor kalibrasi" maxLength={16} /></YStack> : null}
          {step === 'capture' || step === 'validate' ? <AppButton onPress={() => void submit(step === 'capture' ? 'CAPTURE' : 'VALIDATE')} disabled={busy || (step === 'capture' && !['tare', 'reference', 'empty', 'full'].includes(anchor))} ariaLabel={step === 'capture' ? 'Ambil sampel' : 'Validasi sampel'}>{step === 'capture' ? 'Ambil sampel' : 'Validasi'}</AppButton> : null}
          {step === 'preview' ? <AppButton onPress={() => void submit('PREVIEW')} disabled={busy} ariaLabel="Buat preview">Buat preview</AppButton> : null}
          {step === 'commit' ? <AppButton onPress={() => void submit('COMMIT', { expected_profile_hash: preview?.proposal_hash })} disabled={busy || !preview} ariaLabel="Commit profil">Commit profil</AppButton> : null}
          {step !== 'done' ? <AppButton variant="danger" onPress={() => void submit('CANCEL')} disabled={busy || !sessionId} ariaLabel="Batalkan kalibrasi">Batalkan</AppButton> : null}
          <AppButton variant="ghost" onPress={onDiagnosticRefresh} disabled={busy} ariaLabel="Perbarui bukti sensor">Perbarui bukti</AppButton>
        </XStack>
        {preview ? <Card borderWidth={1} borderColor="$primarySoft" backgroundColor="$primarySoft" padding="$3" borderRadius="$3"><Text color="$color" fontWeight="700">Preview proposal</Text><Text color="$colorMuted" fontSize="$2">Hash proposal: {preview.proposal_hash}</Text><Text color="$colorMuted" fontSize="$2">Versi: {preview.profile_version} · evidence {preview.evidence_count} · kedaluwarsa {preview.expires_at}</Text><Text color="$color" fontSize="$2">{JSON.stringify(preview.parameters)}</Text></Card> : null}
      </>}
      {error ? <Text role="alert" color="$statusSafetyAlert">{error}</Text> : null}
      {busy ? <Text color="$colorMuted">Memproses secara bounded...</Text> : null}
    </Card> : null}
  </>;
}
