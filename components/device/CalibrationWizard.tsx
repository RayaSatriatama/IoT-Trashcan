import { useState } from 'react';
import { Card, Input, Text, XStack, YStack } from 'tamagui';

import { createCalibrationApi, type CalibrationApi, type CalibrationCommandRequest, type CalibrationPreview, type CalibrationSensorId, type CalibrationState, type SensorDiagnostic } from '../../lib/api/calibration';
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
};

export type BleCalibrationAdapter = {
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

export function CalibrationWizard({ deviceId, sensorId, diagnostic, api, apiFactory = createCalibrationApi, bleClient = null, bleConnected = false, onConnectBle, transport = 'https', onDiagnosticRefresh }: CalibrationWizardProps) {
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
  const [sessionState, setSessionState] = useState(initialWizardStatus(diagnostic).state);
  const [securedDiagnostic, setSecuredDiagnostic] = useState<SensorDiagnostic | null>(null);
  const activeDiagnostic = securedDiagnostic ?? diagnostic;
  const ready = canBeginCalibration(activeDiagnostic);

  const loadSecuredDiagnostic = async () => {
    const client = api ?? (operatorToken.trim() ? apiFactory({ bearerToken: operatorToken.trim() }) : null);
    if (!client) { setError('Masukkan token sesi operator untuk memuat diagnostik terautentikasi.'); return; }
    setBusy(true); setError(null);
    try {
      const response = await client.getDiagnostics(deviceId, sensorId);
      if (!response.diagnostic) throw new Error('Diagnostik terautentikasi tidak tersedia');
      setSecuredDiagnostic(response.diagnostic);
    } catch (cause) { setError(redactCalibrationError(cause)); }
    finally { setBusy(false); }
  };

  const submit = async (operation: CalibrationCommandRequest['operation'], extra: Partial<CalibrationCommandRequest> = {}) => {
    const client = api ?? (transport === 'https' && operatorToken.trim() ? apiFactory({ bearerToken: operatorToken.trim() }) : null);
    if (!client && !bleClient) { setError('Sesi operator diperlukan untuk mode HTTPS, atau hubungkan BLE lokal.'); return; }
    setError(null); setBusy(true);
    try {
      if (transport === 'ble' && bleClient) {
        const output = operation === 'START' ? await bleClient.calibrationStart(sensorId)
          : operation === 'SET_CRITERIA' ? await bleClient.calibrationCriteria(sensorId, extra.criteria ?? {})
          : operation === 'CAPTURE' ? await bleClient.calibrationCapture(sensorId, anchor as 'tare' | 'reference' | 'empty' | 'full')
          : operation === 'VALIDATE' ? await bleClient.calibrationValidate(sensorId)
          : operation === 'PREVIEW' ? await bleClient.calibrationPreview(sensorId)
          : operation === 'COMMIT' ? await bleClient.calibrationCommit(sensorId)
          : await bleClient.calibrationCancel();
        if (output) setPreview(null);
        const stateByOperation: Record<CalibrationCommandRequest['operation'], CalibrationState> = { START: 'CAPTURING', SET_CRITERIA: 'CAPTURING', CAPTURE: 'VALIDATING', VALIDATE: 'PREVIEW_READY', PREVIEW: 'PREVIEW_READY', COMMIT: 'COMMITTED', CANCEL: 'CANCELLED', ROLLBACK: 'FAILED' };
        setSessionState(stateByOperation[operation]);
        setStep(nextStep(stateByOperation[operation], step));
        onDiagnosticRefresh?.();
        return;
      }
      if (!client) throw new Error('Operator session is unavailable');
      const response = await client.submit({ device_id: deviceId, sensor_id: sensorId, operation, session_id: sessionId ?? undefined, ...extra });
      if (response.session_id) setSessionId(response.session_id);
      if (response.preview) setPreview(response.preview);
      const commandState = response.command?.status ?? response.command?.state ?? 'IDLE';
      setSessionState(commandState);
      const next = nextStep(commandState, step);
      setStep(next);
      if (commandState === 'COMMITTED' || commandState === 'CANCELLED') onDiagnosticRefresh?.();
    } catch (cause) { setError(redactCalibrationError(cause)); }
    finally { setBusy(false); }
  };

  const start = () => {
    const ref = fieldValue(reference);
    if (ref === undefined) { setError('Referensi fisik wajib diisi dengan angka dan unit yang sesuai.'); return; }
    void submit('START', { references: { primary: ref } });
  };
  const configure = () => {
    const value = fieldValue(criteria);
    if (value === undefined) { setError('Kriteria wajib diisi sebelum capture.'); return; }
    void submit('SET_CRITERIA', { criteria: { stability_window: value } });
  };

  return <>
    <AppButton variant="secondary" size="sm" onPress={() => { setOpen(true); setError(null); }} ariaLabel={`Kalibrasi ${sensorId}`}>Kalibrasi sensor</AppButton>
    {open ? <Card role="dialog" aria-label="Wizard kalibrasi sensor" borderWidth={1} borderColor="$borderColor" backgroundColor="$surface" borderRadius="$4" padding="$4" gap="$3">
      <XStack justifyContent="space-between" alignItems="center" gap="$3"><YStack flex={1} gap="$1"><Text color="$color" fontSize="$5" fontWeight="800">Kalibrasi {sensorId}</Text><Text color="$colorMuted" fontSize="$2">Perangkat {deviceId} · transport {transport === 'ble' ? 'BLE lokal' : 'HTTPS Wi-Fi'}</Text></YStack><AppButton variant="ghost" size="sm" onPress={() => setOpen(false)} ariaLabel="Tutup wizard kalibrasi">Tutup</AppButton></XStack>
      <Text color="$colorMuted" fontSize="$2">Langkah: {step} · {calibrationStateLabel(sessionState)}</Text>
      {transport === 'ble' && !bleConnected ? <YStack gap="$2"><Text color="$statusWaspada">BLE belum terhubung. Browser harus mendukung Web Bluetooth pada HTTPS.</Text><AppButton onPress={() => void onConnectBle?.()} disabled={busy || !onConnectBle} ariaLabel="Hubungkan BLE kalibrasi">Hubungkan BLE</AppButton></YStack> : null}
      {transport === 'https' && !api ? <YStack gap="$2"><Text color="$color" fontWeight="700">Sesi operator</Text><Input value={operatorToken} onChangeText={(value) => { setOperatorToken(value); setSecuredDiagnostic(null); }} placeholder="Token sesi operator (tidak disimpan)" secureTextEntry aria-label="Token sesi operator" maxLength={512} autoCapitalize="none" /><AppButton variant="secondary" size="sm" onPress={() => void loadSecuredDiagnostic()} disabled={busy || !operatorToken.trim()} ariaLabel="Muat diagnostik terautentikasi">Muat diagnostik terautentikasi</AppButton></YStack> : null}
      {!activeDiagnostic ? <Text color="$colorMuted">Diagnostik belum tersedia. Tidak ada bukti sintetis yang digunakan.</Text> : <>
        <Text color={ready ? '$statusNormal' : '$statusWaspada'}>{ready ? 'Bukti raw segar tersedia untuk validasi.' : 'Bukti belum layak untuk capture. Periksa quality, CRC, identitas, dan freshness.'}</Text>
        <YStack gap="$2"><Text color="$color" fontWeight="700">Referensi fisik</Text><Input value={reference} onChangeText={setReference} placeholder="Nilai referensi dengan unit terdokumentasi" keyboardType="numeric" aria-label="Nilai referensi fisik" maxLength={32} /></YStack>
        {(step === 'criteria' || step === 'capture' || step === 'validate' || step === 'preview' || step === 'commit') ? <YStack gap="$2"><Text color="$color" fontWeight="700">Kriteria eksplisit</Text><Input value={criteria} onChangeText={setCriteria} placeholder="Window/stabilitas sesuai kemampuan sensor" keyboardType="numeric" aria-label="Kriteria stabilitas" maxLength={32} /></YStack> : null}
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
