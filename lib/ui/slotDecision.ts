import { evaluateTransportPriority, type TransportPriority } from './transportFuzzy';

/**
 * Collection decision from the five configured factors.
 *
 * Inputs: fill (%), weight utilization (%), NH3-equivalent (ppm), accumulation
 * (hours) and temperature (°C). Output: a 0-100 score and one of the three
 * operator categories.
 *
 * The label mapping is deliberately simple and documented: Rendah -> Normal,
 * Sedang -> Waspada, Tinggi/Kritis -> Angkat. Missing inputs leave the decision
 * unavailable instead of defaulting to a number.
 */
export type SlotDecisionStatus = 'Normal' | 'Waspada' | 'Angkat';

export interface SlotDecision {
  score: number | null;
  status: SlotDecisionStatus | null;
  label: string | null;
  decision: string | null;
  missing: readonly string[];
  /** Full evaluation trace, kept so the UI can show how the score was formed. */
  priority: TransportPriority;
}

export interface SlotDecisionInputs {
  fill: number | null | undefined;
  weightUtilization: number | null | undefined;
  ammoniaPpm: number | null | undefined;
  accumulationHours: number | null | undefined;
  temperatureC: number | null | undefined;
}

const finite = (value: number | null | undefined): number | null =>
  typeof value === 'number' && Number.isFinite(value) ? value : null;

export function computeSlotDecision(inputs: SlotDecisionInputs): SlotDecision {
  const priority = evaluateTransportPriority({
    fill: finite(inputs.fill),
    weight_utilization: finite(inputs.weightUtilization),
    ammonia: finite(inputs.ammoniaPpm),
    accumulation: finite(inputs.accumulationHours),
    temperature: finite(inputs.temperatureC),
  });
  if (priority.state !== 'READY' || priority.score === null) {
    return { score: null, status: null, label: null, decision: null, missing: priority.missingInputs, priority };
  }
  const status: SlotDecisionStatus = priority.label === 'Rendah' ? 'Normal' : priority.label === 'Sedang' ? 'Waspada' : 'Angkat';
  return { score: priority.score, status, label: priority.label, decision: priority.decision, missing: [], priority };
}
