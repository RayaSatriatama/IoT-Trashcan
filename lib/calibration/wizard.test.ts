import { describe, expect, it } from 'vitest';

import { canBeginCalibration, calibrationStateLabel, commandSessionState, hasValidatedPreview, initialWizardStatus, nextStep, nextStepAfterOperation, redactCalibrationError } from './wizard';
import type { CalibrationCommand, CalibrationPreview, SensorDiagnostic } from '../api/calibration';

const diagnostic: SensorDiagnostic = {
  sensor_id: 'hx711_organic', stream: 'organic', quality: 'UNCALIBRATED', reason: 'PROFILE_MISSING',
  raw: { present: true, value: 123, unit: 'counts' }, converted_value: null, converted_unit: 'kg', fresh: true,
  age_ms: 20, last_sequence: '4', profile_present: false, profile_version: null, profile_hash: null,
  calibration_state: 'IDLE', accepted_samples: 0, rejected_samples: 0,
};

describe('calibration wizard contract', () => {
  it('rejects missing, stale, and failed evidence before capture', () => {
    expect(canBeginCalibration(null)).toBe(false);
    expect(canBeginCalibration({ ...diagnostic, raw: { present: false } })).toBe(false);
    expect(canBeginCalibration({ ...diagnostic, fresh: false })).toBe(false);
    expect(canBeginCalibration({ ...diagnostic, quality: 'CRC_ERROR' })).toBe(false);
    expect(canBeginCalibration(diagnostic)).toBe(true);
  });

  it('maps session state to bounded operator labels and steps', () => {
    expect(initialWizardStatus(diagnostic).step).toBe('diagnostics');
    expect(nextStep('IDLE', 'diagnostics')).toBe('criteria');
    expect(nextStep('CAPTURING', 'criteria')).toBe('validate');
    expect(nextStep('PREVIEW_READY', 'preview')).toBe('commit');
    expect(nextStep('COMMITTED', 'commit')).toBe('done');
    expect(calibrationStateLabel('EXPIRED')).toBe('Kedaluwarsa');
    expect(calibrationStateLabel('QUEUED')).toBe('Menunggu perangkat');
    expect(nextStep('DELIVERED', 'criteria')).toBe('criteria');
    expect(nextStepAfterOperation('START', 'ACCEPTED', 'diagnostics')).toBe('criteria');
    expect(nextStepAfterOperation('SET_CRITERIA', 'ACCEPTED', 'criteria')).toBe('capture');
    expect(nextStepAfterOperation('CAPTURE', 'CAPTURING', 'capture')).toBe('validate');
    expect(nextStepAfterOperation('VALIDATE', 'VALIDATED', 'validate')).toBe('preview');
    expect(nextStepAfterOperation('PREVIEW', 'PREVIEW_READY', 'preview')).toBe('commit');
    expect(nextStepAfterOperation('COMMIT', 'COMMITTED', 'commit')).toBe('done');
    expect(nextStepAfterOperation('START', 'FAILED', 'diagnostics')).toBe('diagnostics');
  });

  it('redacts secret-shaped error details', () => {
    expect(redactCalibrationError(new Error('token=abc password=hunter2'))).toBe('token=[REDACTED] password=[REDACTED]');
  });

  it('keeps command completion separate from the authoritative session state', () => {
    const command: CalibrationCommand = {
      command_id: 'commit-1', device_id: 'SIGAP-01', sensor_id: 'hx711_organic', operation: 'commit',
      status: 'COMMITTED', state: 'PREVIEW_READY', expires_at: '2026-10-09T12:00:00Z',
    };
    expect(commandSessionState(command)).toBe('PREVIEW_READY');
  });

  it('requires a complete, fresh device proposal before enabling commit', () => {
    const preview: CalibrationPreview = {
      sensor_id: 'ds18b20_b3', proposal_hash: 'sha256:proposal', profile_version: '2',
      parameters: { offset_centi_c: 10 }, evidence_count: 3, evidence_hash: 'sha256:evidence',
      expires_at: '2030-01-01T00:00:00.000Z',
    };
    expect(hasValidatedPreview(preview, 'ds18b20_b3', Date.parse('2029-01-01T00:00:00.000Z'))).toBe(true);
    expect(hasValidatedPreview({ ...preview, evidence_hash: '' }, 'ds18b20_b3', Date.parse('2029-01-01T00:00:00.000Z'))).toBe(false);
    expect(hasValidatedPreview({ ...preview, expires_at: '2028-01-01T00:00:00.000Z' }, 'ds18b20_b3', Date.parse('2029-01-01T00:00:00.000Z'))).toBe(false);
    expect(hasValidatedPreview(preview, 'sht31_44', Date.parse('2029-01-01T00:00:00.000Z'))).toBe(false);
  });
});
