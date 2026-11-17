/**
 * Transport-priority fuzzy sets for the decision inputs.
 *
 * Membership parameters follow the owner-supplied design table (Rancangan awal,
 * memerlukan validasi) for fill, weight utilization, NH3-equivalent and
 * accumulation. The rule base is not part of that table, so it is derived
 * deterministically: the severity indices of the active sets are summed, scaled
 * by the number of inputs and mapped onto the four output sets, with Mamdani
 * min/max implication and a 101-point centroid.
 *
 * The NH3 input keeps exactly the same treatment as every other input: one
 * three-set triangular/trapezoidal partition and the same AND/min rule strength.
 * Temperature is the fifth input; its sets are a design assumption because the
 * owner table did not publish them.
 *
 * Nothing here is a production safety model, gas calibration, or physical
 * accuracy claim. A missing input never becomes zero; it makes the aggregate
 * unavailable.
 */

export type TransportInputId = 'fill' | 'weight_utilization' | 'ammonia' | 'accumulation' | 'temperature';

export interface TransportSetSpec {
  id: string;
  shape: 'triangle' | 'trapezoid';
  points: readonly number[];
}

export interface TransportInputSpec {
  id: TransportInputId;
  label: string;
  unit: string;
  domain: readonly [number, number];
  sets: readonly TransportSetSpec[];
}

export const TRANSPORT_INPUTS: readonly TransportInputSpec[] = [
  {
    id: 'fill', label: 'Keterisian', unit: '%', domain: [0, 100],
    sets: [
      { id: 'Rendah', shape: 'trapezoid', points: [0, 0, 25, 45] },
      { id: 'Sedang', shape: 'triangle', points: [30, 55, 80] },
      { id: 'Tinggi', shape: 'trapezoid', points: [65, 85, 100, 100] },
    ],
  },
  {
    id: 'weight_utilization', label: 'Utilisasi berat', unit: '%', domain: [0, 100],
    sets: [
      { id: 'Ringan', shape: 'trapezoid', points: [0, 0, 20, 40] },
      { id: 'Sedang', shape: 'triangle', points: [25, 50, 75] },
      { id: 'Berat', shape: 'trapezoid', points: [60, 80, 100, 100] },
    ],
  },
  {
    // Same three-set partition and the same rule handling as the other inputs.
    id: 'ammonia', label: 'Amonia', unit: 'ppm', domain: [0, 300],
    sets: [
      { id: 'Rendah', shape: 'trapezoid', points: [0, 0, 10, 20] },
      { id: 'Sedang', shape: 'triangle', points: [10, 25, 40] },
      { id: 'Tinggi', shape: 'trapezoid', points: [30, 50, 300, 300] },
    ],
  },
  {
    // Owner final configuration puts this input in hours, so the same trapezoid
    // parameters are read as hours (short < 20 h, long > 80 h).
    id: 'accumulation', label: 'Waktu akumulasi', unit: 'jam', domain: [0, 100],
    sets: [
      { id: 'Singkat', shape: 'trapezoid', points: [0, 0, 20, 40] },
      { id: 'Sedang', shape: 'triangle', points: [25, 50, 75] },
      { id: 'Lama', shape: 'trapezoid', points: [60, 80, 100, 100] },
    ],
  },
  {
    // Design assumption: no owner table for temperature. A warm bin means active
    // decomposition and stronger odour, so warmth raises the priority.
    id: 'temperature', label: 'Suhu', unit: '°C', domain: [0, 60],
    sets: [
      { id: 'Dingin', shape: 'trapezoid', points: [0, 0, 15, 25] },
      // Named like the other mid sets so it cannot be confused with a decision status.
      { id: 'Sedang', shape: 'triangle', points: [20, 30, 40] },
      { id: 'Panas', shape: 'trapezoid', points: [35, 45, 60, 60] },
    ],
  },
];

export const TRANSPORT_OUTPUT_SETS: readonly TransportSetSpec[] = [
  { id: 'Rendah', shape: 'trapezoid', points: [0, 0, 30, 40] },
  { id: 'Sedang', shape: 'triangle', points: [25, 45, 65] },
  { id: 'Tinggi', shape: 'triangle', points: [50, 70, 90] },
  { id: 'Kritis', shape: 'trapezoid', points: [75, 90, 100, 100] },
];

export const TRANSPORT_OUTPUT_LABELS = [
  { id: 'Rendah', minimumInclusive: 0, decision: 'Belum perlu diangkut' },
  { id: 'Sedang', minimumInclusive: 25, decision: 'Perlu dipantau' },
  { id: 'Tinggi', minimumInclusive: 50, decision: 'Direkomendasikan diangkut' },
  { id: 'Kritis', minimumInclusive: 75, decision: 'Penanganan segera' },
] as const;

/** Triangular or trapezoidal membership, zero outside the support. */
export function transportMembership(spec: TransportSetSpec, value: number): number {
  if (!Number.isFinite(value)) return 0;
  const points = spec.points;
  if (spec.shape === 'triangle') {
    if (value <= points[0] || value >= points[2]) return 0;
    if (value === points[1]) return 1;
    if (value < points[1]) return (value - points[0]) / (points[1] - points[0]);
    return (points[2] - value) / (points[2] - points[1]);
  }
  if (value < points[0] || value > points[3]) return 0;
  if (points[0] === points[1] && value <= points[1]) return 1;
  if (value < points[1]) return (value - points[0]) / (points[1] - points[0]);
  if (value <= points[2]) return 1;
  if (points[2] === points[3] && value >= points[2]) return 1;
  return (points[3] - value) / (points[3] - points[2]);
}

export interface TransportMembershipResult {
  input: TransportInputId;
  label: string;
  set: string | null;
  degree: number;
  memberships: { set: string; degree: number }[];
}

/** Strongest set per input, or no set when the value is not available. */
export function transportInputMembership(spec: TransportInputSpec, value: number | null | undefined): TransportMembershipResult {
  if (typeof value !== 'number' || !Number.isFinite(value)) {
    return { input: spec.id, label: spec.label, set: null, degree: 0, memberships: [] };
  }
  const clamped = Math.min(spec.domain[1], Math.max(spec.domain[0], value));
  const memberships = spec.sets.map(set => ({ set: set.id, degree: transportMembership(set, clamped) }));
  const strongest = memberships.reduce((best, item) => item.degree > best.degree ? item : best, memberships[0]);
  return {
    input: spec.id, label: spec.label,
    set: strongest.degree > 0 ? strongest.set : null,
    degree: strongest.degree, memberships,
  };
}

export interface TransportInputs {
  fill: number | null;
  weight_utilization: number | null;
  ammonia: number | null;
  accumulation: number | null;
  temperature: number | null;
}

/** One fired rule: the AND (minimum) of its antecedent degrees. */
export interface TransportRuleTrace {
  output: string;
  strength: number;
  antecedents: { input: TransportInputId; label: string; set: string; degree: number }[];
}

export interface TransportAggregateTrace {
  /** Aggregated (max over rules) firing level of each output set. */
  levels: { set: string; level: number }[];
  /** Area under the aggregated membership curve and its first moment. */
  area: number;
  moment: number;
  centroid: number;
  grid: number;
}

export interface TransportPriority {
  state: 'READY' | 'UNAVAILABLE';
  score: number | null;
  label: 'Rendah' | 'Sedang' | 'Tinggi' | 'Kritis' | null;
  decision: string | null;
  missingInputs: TransportInputId[];
  memberships: TransportMembershipResult[];
  rules: TransportRuleTrace[];
  aggregate: TransportAggregateTrace | null;
}

/**
 * Severity of a rule from the sum of its antecedent set indices.
 *
 * `scaled` puts the sum on the four-input scale, so a 4-input evaluation keeps
 * the exact table [0,0,1,1,2,2,3,3,3] while five inputs spread the same range
 * over one more step. Clamped to the four output sets.
 */
export function ruleSeverity(sum: number, inputCount: number): number {
  if (inputCount <= 0) return 0;
  const scaled = (sum * 4) / inputCount;
  return Math.min(3, Math.max(0, Math.ceil((scaled - 1) / 2)));
}

export function evaluateTransportPriority(inputs: TransportInputs): TransportPriority {
  const memberships = TRANSPORT_INPUTS.map(spec => transportInputMembership(spec, inputs[spec.id]));
  const missingInputs = memberships.filter(item => item.set === null).map(item => item.input);
  const base: TransportPriority = {
    state: 'UNAVAILABLE', score: null, label: null, decision: null,
    missingInputs, memberships, rules: [], aggregate: null,
  };
  if (missingInputs.length > 0) return base;

  const inputCount = memberships.length;
  const setCount = 3;
  const strengths = [0, 0, 0, 0];
  const rules: TransportRuleTrace[] = [];
  const indices = new Array<number>(inputCount).fill(0);

  // Odometer over every combination of the three sets of every input.
  for (;;) {
    let strength = 1;
    let sum = 0;
    for (let index = 0; index < inputCount; index += 1) {
      strength = Math.min(strength, memberships[index].memberships[indices[index]].degree);
      sum += indices[index];
    }
    if (strength > 0) {
      const consequent = ruleSeverity(sum, inputCount);
      strengths[consequent] = Math.max(strengths[consequent], strength);
      rules.push({
        output: TRANSPORT_OUTPUT_SETS[consequent].id,
        strength,
        antecedents: indices.map((setIndex, index) => ({
          input: memberships[index].input,
          label: memberships[index].label,
          set: memberships[index].memberships[setIndex].set,
          degree: memberships[index].memberships[setIndex].degree,
        })),
      });
    }
    let position = inputCount - 1;
    while (position >= 0 && indices[position] === setCount - 1) {
      indices[position] = 0;
      position -= 1;
    }
    if (position < 0) break;
    indices[position] += 1;
  }

  const minimum = 0, maximum = 100, grid = 101;
  const step = (maximum - minimum) / (grid - 1);
  let area = 0, moment = 0, previous = 0, previousX = minimum;
  for (let index = 0; index < grid; index += 1) {
    const x = minimum + step * index;
    let aggregated = 0;
    for (let setIndex = 0; setIndex < TRANSPORT_OUTPUT_SETS.length; setIndex += 1) {
      aggregated = Math.max(aggregated, Math.min(transportMembership(TRANSPORT_OUTPUT_SETS[setIndex], x), strengths[setIndex]));
    }
    if (index !== 0) {
      const width = x - previousX;
      area += width * (previous + aggregated) * 0.5;
      moment += width * (previousX * previous + x * aggregated) * 0.5;
    }
    previous = aggregated;
    previousX = x;
  }
  const aggregate: TransportAggregateTrace = {
    levels: TRANSPORT_OUTPUT_SETS.map((set, index) => ({ set: set.id, level: strengths[index] })),
    area, moment, centroid: area > 1e-4 ? moment / area : 0, grid,
  };
  rules.sort((a, b) => b.strength - a.strength);
  if (!Number.isFinite(area) || area <= 1e-4) return { ...base, rules, aggregate };
  const centroid = moment / area;
  if (!Number.isFinite(centroid)) return { ...base, rules, aggregate };
  const label = [...TRANSPORT_OUTPUT_LABELS].reverse().find(item => centroid >= item.minimumInclusive)!;
  return {
    state: 'READY', score: Math.min(100, Math.max(0, centroid)), label: label.id,
    decision: label.decision, missingInputs: [], memberships, rules, aggregate,
  };
}

/** Maximum organic weight declared by the owner; editable from the UI. */
export const ORGANIC_CAPACITY_KG = 2;

/** Weight utilization percentage from an absolute weight, or null when absent. */
export function weightUtilizationPercent(weightKg: number | null | undefined, capacityKg = ORGANIC_CAPACITY_KG): number | null {
  if (typeof weightKg !== 'number' || !Number.isFinite(weightKg)) return null;
  if (!(capacityKg > 0)) return null;
  return Math.abs(weightKg) / capacityKg * 100;
}
