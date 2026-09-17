import type { CanonicalStatus } from './api/types';

export interface StatusPresentation {
  label: string;
  token: `status${string}`;
}

const presentations: Record<CanonicalStatus, StatusPresentation> = {
  Normal: { label: 'Normal', token: 'statusNormal' },
  Waspada: { label: 'Waspada', token: 'statusWaspada' },
  Angkat: { label: 'Perlu diangkat', token: 'statusAngkat' },
  'Safety Alert': { label: 'Peringatan keselamatan', token: 'statusSafetyAlert' },
  Offline: { label: 'Offline', token: 'statusOffline' },
  'Data Invalid': { label: 'Data tidak valid', token: 'statusDataInvalid' },
};

export function isCanonicalStatus(value: string | null | undefined): value is CanonicalStatus {
  return value !== null && value !== undefined && value in presentations;
}

export function getStatusPresentation(status: CanonicalStatus): StatusPresentation {
  return presentations[status];
}
