import { describe, expect, it } from 'vitest';
import type { CalibrationPreview } from '../../lib/api/calibration';
import { canCommitApprovedPreview, previewAgreementKey } from '../../components/device/CalibrationWizard';

const currentPreview: CalibrationPreview = {
  sensor_id: 'mq7',
  proposal_hash: 'proposal-current',
  profile_version: '12',
  parameters: { adc_reference_mv: 3300 },
  evidence_count: 4,
  evidence_hash: 'evidence-current',
  expires_at: '2030-01-01T00:00:00.000Z',
};

const keyFor = (preview: CalibrationPreview) => previewAgreementKey('device-current', 'mq7', preview);

describe('calibration proposal approval', () => {
  it('allows commit only for the exact current device and sensor proposal before expiry', () => {
    expect(canCommitApprovedPreview(currentPreview, 'device-current', 'mq7', keyFor(currentPreview), Date.parse('2029-12-31T23:59:59.000Z'))).toBe(true);
  });

  it('rejects approval reused for another context or changed evidence/proposal', () => {
    const changed = { ...currentPreview, evidence_hash: 'evidence-next' };
    expect(canCommitApprovedPreview(currentPreview, 'device-next', 'mq7', keyFor(currentPreview), Date.parse('2029-12-31T23:59:59.000Z'))).toBe(false);
    expect(canCommitApprovedPreview(changed, 'device-current', 'mq7', keyFor(currentPreview), Date.parse('2029-12-31T23:59:59.000Z'))).toBe(false);
  });

  it('rejects an expired proposal at the click-time clock boundary', () => {
    expect(canCommitApprovedPreview(currentPreview, 'device-current', 'mq7', keyFor(currentPreview), Date.parse(currentPreview.expires_at))).toBe(false);
  });
});
