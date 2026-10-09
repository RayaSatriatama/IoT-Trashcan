import { describe, expect, it } from 'vitest';

import { canBeginCalibration, calibrationStateLabel, initialWizardStatus, nextStep, redactCalibrationError } from './wizard';
import type { SensorDiagnostic } from '../api/calibration';

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
  });

  it('redacts secret-shaped error details', () => {
    expect(redactCalibrationError(new Error('token=abc password=hunter2'))).toBe('token=[REDACTED] password=[REDACTED]');
  });
});

