import { useCallback, useEffect, useState } from 'react';
import { ORGANIC_CAPACITY_KG } from './transportFuzzy';

/**
 * Operator-editable reference capacity for the organic slot.
 *
 * The value only drives the presentation (the `N kg / M kg` bar, the weight
 * utilization percentage and the fuzzy weight input). The default is 2 kg. It is stored in localStorage per device so a reload
 * keeps the operator's setting, and it never changes firmware or the decision
 * inputs on the device side.
 */
const STORAGE_PREFIX = 'sigap.organic.capacity.';
export const MIN_CAPACITY_KG = 0.1;
export const MAX_CAPACITY_KG = 100;

/** A stored value from an older capacity default is migrated to the new one. */
const LEGACY_CAPACITIES_KG = [5];

export interface OrganicCapacity {
  capacityKg: number;
  setCapacityKg: (value: number) => void;
  reset: () => void;
  isDefault: boolean;
}

const clamp = (value: number) => Math.min(MAX_CAPACITY_KG, Math.max(MIN_CAPACITY_KG, value));

function read(key: string): number {
  if (typeof window === 'undefined') return ORGANIC_CAPACITY_KG;
  try {
    const raw = window.localStorage.getItem(key);
    if (!raw) return ORGANIC_CAPACITY_KG;
    const parsed = Number.parseFloat(raw);
    if (!Number.isFinite(parsed)) return ORGANIC_CAPACITY_KG;
    // Only an exact legacy default is migrated; an operator's own value stays.
    if (LEGACY_CAPACITIES_KG.includes(parsed)) return ORGANIC_CAPACITY_KG;
    return clamp(parsed);
  } catch {
    return ORGANIC_CAPACITY_KG;
  }
}

export function useOrganicCapacity(deviceId: string): OrganicCapacity {
  const key = `${STORAGE_PREFIX}${deviceId}`;
  const [capacityKg, setValue] = useState(() => read(key));

  useEffect(() => {
    if (typeof window === 'undefined') return;
    try {
      window.localStorage.setItem(key, String(capacityKg));
    } catch {
      /* storage unavailable; the value then only lives in memory */
    }
  }, [key, capacityKg]);

  const setCapacityKg = useCallback((value: number) => {
    if (!Number.isFinite(value)) return;
    setValue(clamp(value));
  }, []);
  const reset = useCallback(() => setValue(ORGANIC_CAPACITY_KG), []);

  return { capacityKg, setCapacityKg, reset, isDefault: Math.abs(capacityKg - ORGANIC_CAPACITY_KG) < 1e-9 };
}

/** Keeps the `N kg / M kg` label readable: 2 -> "2", 4.35 -> "4,35". */
export function formatCapacity(value: number): string {
  const rounded = Number.isInteger(value) ? String(value) : String(Math.round(value * 100) / 100);
  return rounded.replace('.', ',');
}
