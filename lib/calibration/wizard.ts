import type { CalibrationState, SensorDiagnostic } from '../api/calibration';

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
  if (!diagnostic.raw.present) return false;
  if (!diagnostic.fresh) return false;
  if (diagnostic.quality === 'TIMEOUT' || diagnostic.quality === 'DISCONNECTED' || diagnostic.quality === 'CRC_ERROR' || diagnostic.quality === 'INVALID') return false;
  return true;
}

export function calibrationStateLabel(state: CalibrationState): string {
  const labels: Record<CalibrationState, string> = {
    IDLE: 'Belum dimulai', CAPTURING: 'Mengumpulkan bukti', VALIDATING: 'Memvalidasi',
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
  if (state === 'IDLE' && current === 'diagnostics') return 'criteria';
  return current;
}

export function redactCalibrationError(error: unknown): string {
  const value = error instanceof Error ? error.message : String(error ?? 'Operasi gagal');
  return value.replace(/(password|token|secret|authorization)\s*[:=]\s*[^\s,;]+/giu, (_match, key: string) => `${key}=[REDACTED]`);
}
