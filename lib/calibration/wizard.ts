import type { CalibrationCommand, CalibrationPreview, CalibrationSensorId, CalibrationState, SensorDiagnostic } from '../api/calibration';

export type CalibrationWizardStep = 'diagnostics' | 'criteria' | 'capture' | 'validate' | 'preview' | 'commit' | 'done';

export type CalibrationWizardStatus = {
  step: CalibrationWizardStep;
  sessionId: string | null;
  state: CalibrationState;
  busy: boolean;
  error: string | null;
  diagnostic: SensorDiagnostic | null;
};

export function initialWizardStatus(diagnostic: SensorDiagnostic | null = null): CalibrationWizardStatus {
  return { step: 'diagnostics', sessionId: null, state: 'IDLE', busy: false, error: null, diagnostic };
}

export function canBeginCalibration(diagnostic: SensorDiagnostic | null): boolean {
  if (!diagnostic) return false;
  const raw = diagnostic.raw;
  if (!raw?.present) return false;
  if (!diagnostic.fresh) return false;
  if (!['READY', 'CALIBRATING', 'UNCALIBRATED'].includes(diagnostic.quality)) return false;
  if (raw.identity_verified === false || raw.crc_valid === false ||
      raw.conversion_complete === false || raw.heater_ready === false) return false;
  if (diagnostic.sensor_id === 'ds18b20_b3' &&
      (raw.identity_verified !== true || raw.crc_valid !== true ||
       raw.conversion_complete !== true)) return false;
  if ((diagnostic.sensor_id === 'sht31_44' || diagnostic.sensor_id === 'sht31_45') &&
      (raw.identity_verified !== true ||
       raw.temperature_crc_valid !== true || raw.humidity_crc_valid !== true)) return false;
  if (diagnostic.sensor_id === 'mq7' && raw.heater_ready !== true) return false;
  return true;
}

export function calibrationStateLabel(state: CalibrationState): string {
  const labels: Record<CalibrationState, string> = {
    IDLE: 'Belum dimulai', QUEUED: 'Menunggu perangkat', DELIVERED: 'Diterima perangkat', ACCEPTED: 'Diterima', CAPTURING: 'Mengumpulkan bukti', VALIDATING: 'Memvalidasi', VALIDATED: 'Tervalidasi',
    PREVIEW_READY: 'Preview siap', COMMITTING: 'Menyimpan profil', COMMITTED: 'Profil aktif',
    CANCELLED: 'Dibatalkan', EXPIRED: 'Kedaluwarsa', FAILED: 'Gagal',
  };
  return labels[state];
}

export function nextStep(state: CalibrationState, current: CalibrationWizardStep): CalibrationWizardStep {
  if (state === 'FAILED' || state === 'EXPIRED' || state === 'CANCELLED') return 'diagnostics';
  if (state === 'COMMITTED') return 'done';
  if (state === 'PREVIEW_READY') return 'commit';
  if (state === 'VALIDATING') return 'preview';
  if (state === 'CAPTURING') return 'validate';
  if (state === 'QUEUED' || state === 'DELIVERED' || state === 'ACCEPTED' || state === 'COMMITTING') return current;
  if (state === 'IDLE' && current === 'diagnostics') return 'criteria';
  return current;
}

/** Maps one device-confirmed command outcome to the next explicit operator
 * step. Delivery status and calibration-session state remain separate, so an
 * ACCEPTED START cannot skip the criteria form or jump to capture. */
export function nextStepAfterOperation(
  operation: string,
  status: CalibrationState,
  current: CalibrationWizardStep,
): CalibrationWizardStep {
  if (status === 'FAILED' || status === 'EXPIRED' || status === 'CANCELLED') return 'diagnostics';
  if (status === 'QUEUED' || status === 'DELIVERED') return current;
  switch (operation.toUpperCase()) {
    case 'START': return 'criteria';
    case 'SET_CRITERIA':
    case 'CRITERIA': return 'capture';
    case 'CAPTURE': return 'validate';
    case 'VALIDATE': return 'preview';
    case 'PREVIEW': return 'commit';
    case 'COMMIT': return status === 'COMMITTED' ? 'done' : current;
    case 'CANCEL': return 'diagnostics';
    default: return nextStep(status, current);
  }
}

/** A command result can be terminal while the calibrated session reports its
 * own authoritative state. Keep those two protocol fields separate. */
export function commandSessionState(command: CalibrationCommand): CalibrationState {
  return command.state ?? command.status;
}

/** A browser may only request COMMIT for the exact unexpired proposal emitted
 * by the selected device and sensor. */
export function hasValidatedPreview(
  preview: CalibrationPreview | null,
  sensorId: CalibrationSensorId,
  nowMs = Date.now(),
): preview is CalibrationPreview {
  if (!preview || preview.sensor_id !== sensorId || preview.evidence_count <= 0) return false;
  if (!preview.proposal_hash.trim() || !preview.profile_version.trim() || !preview.evidence_hash.trim()) return false;
  const expiresAt = Date.parse(preview.expires_at);
  return Number.isFinite(expiresAt) && expiresAt > nowMs;
}

export function redactCalibrationError(error: unknown): string {
  const value = error instanceof Error ? error.message : String(error ?? 'Operasi gagal');
  return value.replace(/(password|token|secret|authorization)\s*[:=]\s*[^\s,;]+/giu, (_match, key: string) => `${key}=[REDACTED]`);
}
